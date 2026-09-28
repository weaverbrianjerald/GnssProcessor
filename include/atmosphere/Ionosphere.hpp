#pragma once

#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include <Eigen/Dense>
#include "math/TimeUtils.hpp"
#include "model/ModelGeometry.hpp"
//#include "products/ProductParser.hpp"
#include <vector>

namespace GnssProcessor {

	/**
	 * @file Ionosphere.hpp
	 * @brief Ionospheric delay helper routines.
	 *
	 * Declares the `Ionosphere` class, which provides the Klobuchar ionospheric
	 * delay and a coarse initial PPP iono estimate. The class depends on
	 * configuration, logging and product data (for the Klobuchar coefficients).
	 * It also declares getTroposphereMapping(), which has no definition; the
	 * tropospheric mapping in use is Troposphere::getTroposphereMapping.
	 */
	class Ionosphere {
	public:
		/**
		 * @brief Construct an Ionosphere helper.
		 *
		 * Initializes the geometry helper and takes the Klobuchar coefficients
		 * from ProductCache; when the cache holds none, parses them from the
		 * header of the configured RNX (RINEX/BRDC) navigation product.
		 *
		 * @param config Read-only configuration reference.
		 * @param log_mgr Log manager used to report warnings, errors and traces.
		 * @param parser Product parser; not used by the constructor.
		 * @param rinex RINEX parser reference passed to the geometry helper.
		 */
		Ionosphere(const GnssConfig& config, LogManager& log_mgr, ProductParser& parser, const RinexParser& rinex);

		/**
		 * @brief Compute tropospheric mapping functions.
		 *
		 * Computes the dry and wet mapping functions for troposphere modelling.
		 * Values are returned through the output parameters `mf_d` (hydrostatic)
		 * and `mf_w` (wet). Declared only: Ionosphere.cpp has no definition, so a
		 * call does not link. Use Troposphere::getTroposphereMapping.
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
		 * initialize PPP states: the Klobuchar vertical delay at L1 evaluated at
		 * the receiver position (no obliquity factor, no pierce-point offset)
		 * when the 8 Klobuchar coefficients are loaded, otherwise 5 meters.
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
		 * for a single-frequency observation, scaled from L1 by (f_L1/f)².
		 * Returns the delay in meters. Precondition: the 8 Klobuchar coefficients
		 * are loaded; the size is not checked.
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
		/** @brief Klobuchar model coefficients: empty, or 8 values (alpha[4], beta[4]). */
		std::vector<double> klobuchar_coeffs_;

		/**
		 * @brief Load Klobuchar coefficients from the configured RNX product.
		 *
		 * Reads the GPSA and GPSB IONOSPHERIC CORR lines from the header of the
		 * RINEX/BRDC navigation file at the configured RNX product path (the file
		 * is read directly, not through ProductParser). Populates
		 * `klobuchar_coeffs_` with 8 values, or leaves it empty on any failure.
		 */
		void loadKlobucharCoeffs();

	};

}