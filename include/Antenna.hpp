#ifndef ANTENNA_HPP
#define ANTENNA_HPP

#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "GnssCorrections.hpp"
#include "ProductParser.hpp"
#include "Planetary.hpp"

namespace GnssProcessor {

	/**
	 * @file Antenna.hpp
	 * @brief Antenna phase-center offset/variation and related corrections.
	 *
	 * This header declares the `Antenna` class which loads antenna models
	 * (PCO/PCV), applies antenna corrections (including phase wind-up),
	 * and exposes utility helpers to query receiver antenna metadata from
	 * the RINEX parser.
	 */

	class RinexParser; // forward-declare

	/**
	 * @brief Provides antenna-related corrections and metadata access.
	 *
	 * The `Antenna` class encapsulates antenna model loading (PCO/PCV),
	 * interpolation of PCV grids, and computation of per-satellite antenna
	 * corrections (in meters) including phase center offsets, phase center
	 * variations and phase wind-up when required.
	 *
	 * The class depends on `GnssConfig` for configuration, `ProductParser`
	 * for product/auxiliary data, `LogManager` for logging, and a
	 * `RinexParser` reference for receiver metadata.
	 */
	class Antenna {
	public:

		/**
		 * @brief Construct an Antenna helper.
		 *
		 * Loads configuration and initializes internal correction objects.
		 * Antenna model data is lazily loaded by `loadAntennaData()` or during
		 * first correction request depending on implementation.
		 *
		 * @param config Configuration reference (read-only).
		 * @param product_parser Product parser used to access auxiliary files.
		 * @param log_mgr Log manager used for warnings/errors.
		 * @param rinex_parser Reference to the RINEX parser for receiver metadata.
		 */
		Antenna(const GnssConfig& config, ProductParser& product_parser, LogManager& log_mgr, const RinexParser& rinex_parser);

		/**
		 * @brief Compute antenna correction for a satellite/receiver observation.
		 *
		 * Computes the combined antenna correction (phase center offset (PCO),
		 * phase center variation (PCV) interpolation and any applicable
		 * phase wind-up) and returns a scalar correction expressed in meters.
		 *
		 * @param satellite Satellite identifier string (e.g. "G01", "E11").
		 * @param freq_type Frequency type string (e.g. "L1", "L2", "E1").
		 * @param system GNSS system enum for the satellite.
		 * @param rx_pos Receiver ECEF position (meters).
		 * @param sat_pos Satellite ECEF position (meters).
		 * @param sat_vel Satellite ECEF velocity (meters/sec).
		 * @param elevation Satellite elevation angle at receiver (radians or degrees depending on caller convention).
		 * @param azimuth Satellite azimuth angle at receiver (radians or degrees depending on caller convention).
		 * @param rx_antenna_type Receiver antenna model/type string (from RINEX header or configuration).
		 * @param time Epoch time at which the correction is computed.
		 * @return Antenna correction in meters. Positive values should be applied to measured ranges as defined by the caller's convention.
		 */
		double getAntennaCorrection(const std::string& satellite, const std::string& freq_type, System system,
			const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos, const Eigen::Vector3d& sat_vel,
			double elevation, double azimuth, const std::string& rx_antenna_type,
			const GnssTime& time) const;

		/**
		 * @brief Get receiver antenna type from the RINEX header.
		 *
		 * Returns the receiver antenna model/type string parsed from the
		 * observation RINEX file header. If no receiver antenna metadata
		 * exists, an empty string is returned.
		 *
		 * @return Receiver antenna type string (e.g. "TRMxxxxx") or empty string if unknown.
		 */
		std::string getReceiverAntennaType() const;
	private:
		/** @brief Logger for informational and diagnostic messages. */
		LogManager& log_manager_;
		/** @brief Read-only configuration settings. */
		const GnssConfig& config_;
		/** @brief GNSS corrections helper (clocks, ERP, relativistic, etc.). */
		GnssCorrections gnss_corrections_;
		/** @brief Loaded antenna models and PCV/PCO data. */
		std::vector<AntennaData> antennas_;
		/** @brief Reference to RINEX parser (for receiver antenna metadata). */
		const RinexParser& rinex_parser_;
		/** @brief Product parser used to find auxiliary files (ATX, etc.). */
		ProductParser& product_parser_;
		/** @brief PPP helper utilities (frequencies, wavelengths, conversions). */
		PppUtils ppp_utils_;
		/** @brief Planetary helper used for Sun position / yaw-steering calculations. */
		Planetary planetary_;

		/**
		 * @brief Loads antenna models (ATX/NGS or other formats) into `antennas_`.
		 *
		 * Internal loader that queries `product_parser_` and parses antenna
		 * model files. Called during construction or on-demand before
		 * computing corrections.
		 */
		void loadAntennaData();

		/**
		 * @brief Interpolate a PCV grid to obtain variation at a given look direction.
		 *
		 * Performs interpolation on a PCV table (2-D grid) using provided
		 * zenith/azimuth coordinates and grid spacings to compute PCV value
		 * in meters.
		 *
		 * @param pcv_table 2-D PCV grid where rows/columns correspond to zenith/azimuth samples.
		 * @param zenith Zenith angle (degrees) or index depending on table convention.
		 * @param azimuth Azimuth angle (degrees) or index depending on table convention.
		 * @param dazi Azimuth grid spacing in degrees.
		 * @param dzen Zenith grid spacing in degrees.
		 * @return Interpolated PCV value in meters.
		 */
		double interpolatePcv(const std::vector<std::vector<double>>& pcv_table, double zenith,
			double azimuth, double dazi, double dzen) const;

	};

}

#endif