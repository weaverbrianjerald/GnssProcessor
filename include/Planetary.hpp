#ifndef PLANETARY_HPP
#define PLANETARY_HPP

#include "LogManager.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "PppTypes.hpp"
//#include "ProductParser.hpp"

namespace GnssProcessor {

	class Planetary {
	public:
		explicit Planetary(LogManager& log_mgr);

		// Compute sun and moon position
		Eigen::Vector3d computeSunPosition(double mjd) const;
		Eigen::Vector3d computeMoonPosition(double mjd) const;
		Eigen::Vector3d rotateVector(const Eigen::Vector3d& vec, double angle, int axis) const;

		// Interpolates Sun position from ephemeris
		Eigen::Vector3d interpolateSunPosition(const GnssTime& time) const;

		// Compute Sun ephemeris for observation epochs
		bool parseSunEphemeris(const std::vector<GnssTime>& epoch_times, std::vector<SunEphemeris>& sun_ephemeris) const;
	private:
		LogManager& log_manager_;                         // Logger for debugging and info
		std::vector<SunEphemeris> sun_ephemeris_;         // Sun ephemeris data
	};

}

#endif