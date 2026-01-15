#ifndef IONOSPHERE_HPP
#define IONOSPHERE_HPP

#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "ModelGeometry.hpp"
//#include "ProductParser.hpp"
#include <vector>

namespace GnssProcessor {

	class Ionosphere {
	public:
		Ionosphere(const GnssConfig& config, LogManager& log_mgr, ProductParser& parser, const RinexParser& rinex);

		// Computes tropospheric mapping functions
		void getTroposphereMapping(double elevation, double& mf_d, double& mf_w) const;

		// Computes initial ionospheric delay (meters) for PPP mode
		double computeInitialIonoDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos) const;

		// Computes Klobuchar ionospheric delay (meters)
		double computeKlobucharDelay(const GnssTime& time, const Eigen::Vector3d& rx_pos,
			const Eigen::Vector3d& sat_pos, double frequency) const;

	private:
		LogManager& log_manager_;                         // Logger for debugging and info
		const GnssConfig& config_;                        // Configuration settings
		Geometry model_geometry_;						  // Satellite geometry
		std::vector<double> klobuchar_coeffs_;            // Klobuchar ionospheric model coefficients

		// Load Klobuchar coefficients from configured product (RINEX/BRDC) file
		void loadKlobucharCoeffs();

	};

}

#endif