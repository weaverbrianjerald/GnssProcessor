#ifndef TROPOSPHERE_HPP
#define TROPOSPHERE_HPP

#include "LogManager.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>

namespace GnssProcessor {

	class Troposphere {
	public:
		Troposphere(const GnssConfig& config, LogManager& log_mgr);

		// Computes tropospheric mapping functions
		void getTroposphereMapping(double elevation, double& mf_d, double& mf_w) const;
		
		// Troposphere
		double computeSaastamoinenZHD(const Eigen::Vector3d& receiver_pos) const;
	private:
		LogManager& log_manager_;                         // Logger for debugging and info
		const GnssConfig& config_;                        // Configuration settings
	};

}

#endif