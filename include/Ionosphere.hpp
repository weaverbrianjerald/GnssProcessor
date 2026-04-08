#pragma once

#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "ModelGeometry.hpp"
//#include "ProductParser.hpp"
#include <vector>

namespace GnssProcessor {

	/**
	 * @file Ionosphere.hpp
	 * @brief Ionospheric and tropospheric helper routines.
	 *
	 * Declares the `Ionosphere` class which provides tropospheric mapping
	 * functions and ionospheric delay estimators (Klobuchar and an initial
	 * PPP iono estimate). The class depends on configuration, logging and
	 * product data (for Klobuchar coefficients).
	 */
	class Ionosphere {
	public:
		/**
		 * @brief Construct an Ionosphere helper.
		 *
		 * Initializes model helpers and loads ionospheric products (e.g. Klobuchar
		 * coefficients) using the provided product parser. A reference to a
		 * RinexParser may be used to obtain coefficients embedded in RINEX headers.
		 *
		 * @param config Read-only configuration reference.
		 * @param log_mgr Log manager used to report warnings, errors and traces.
		 * @param parser Product parser used to load ionospheric products.
		 * @param rinex RINEX parser reference used to access header metadata.
		 */
		Ionosphere(const GnssConfig& config, LogManager& log_mgr, ProductParser& parser, const RinexParser& rinex);

		/**
		 * @brief Compute tropospheric mapping functions.
		 *
		 * Computes the dry and wet mapping functions for troposphere modelling.
		 * Values are returned through the output parameters `mf_d` (hydrostatic)
		 * and `mf_w` (wet).
		 *
		 * @param elevation Satellite elevation angle in degrees.
		 * @param[out] mf_d Hydrostatic (dry) mapping function (unitless).
		 * @param[out] mf_w Wet mapping function (unitless).
		 */
		void getTroposphereMapping(double elevation, double& mf_d, double& mf_w) const;

		/**
		 * @brief Compute an initial ionospheric delay estimate for PPP.
		 *
		 * Provides a coarse initial ionospheric delay (in meters) to help
		 * initialize PPP states. The implementation may use broadcast products,
		 * Klobuchar, or simple geometric heuristics depending on available data.
		 *
		 * @param time GNSS epoch at which the delay is needed.
		 * @param rx_pos Receiver ECEF position (meters).
		 * @return Initial ionospheric delay in meters.
		 */
		double computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const;

		/**
		 * @brief Compute Klobuchar ionospheric delay.
		 *
		 * Implements the standard Klobuchar model to estimate ionospheric delay
		 * for a single-frequency observation. Returns the delay in meters.
		 *
		 * @param time GNSS epoch for the observation.
		 * @param rx_pos Receiver ECEF position (meters).
		 * @param sat_pos Satellite ECEF position (meters).
		 * @param frequency Frequency of the signal in Hz.
		 * @return Estimated Klobuchar ionospheric delay in meters.
		 */
		double computeKlobucharDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
			const Eigen::Vector3d& sat_pos, double frequency) const;

	private:
		/** @brief Logger for informational and diagnostic messages. */
		LogManager& log_manager_;
		/** @brief Read-only configuration settings. */
		const GnssConfig& config_;
		/** @brief Geometry helper which computes satellite geometry and related quantities. */
		Model::Geometry model_geometry_;
		/** @brief Klobuchar model coefficients (typically size 4 or 8 depending on source). */
		std::vector<double> klobuchar_coeffs_;

		/**
		 * @brief Load Klobuchar coefficients from configured products.
		 *
		 * Attempts to read Klobuchar coefficients from navigation/RINEX or
		 * auxiliary product files via the ProductParser provided at
		 * construction. Populates `klobuchar_coeffs_`.
		 */
		void loadKlobucharCoeffs();

	};

}