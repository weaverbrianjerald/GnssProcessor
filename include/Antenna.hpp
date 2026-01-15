#ifndef ANTENNA_HPP
#define ANTENNA_HPP

#include "LogManager.hpp"
#include "PppTypes.hpp"
#include "GnssConfig.hpp"
#include <Eigen/Dense>
#include "TimeUtils.hpp"
#include "GnssCorrections.hpp"
#include "ProductParser.hpp"

namespace GnssProcessor {

	class RinexParser; // forward-declare

	class Antenna {
	public:

		// Construct - removed PppModel& to avoid circular dependency
		Antenna(const GnssConfig& config, ProductParser& product_parser, LogManager& log_mgr, const RinexParser& rinex_parser);

		// Computes antenna corrections (PCO, PCV, phase wind-up) in meters
		double getAntennaCorrection(const std::string& satellite, const std::string& freq_type, System system,
			const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos,
			double elevation, double azimuth, const std::string& rx_antenna_type,
			const GnssTime& time) const;

		// Returns receiver antenna type from RINEX header
		std::string getReceiverAntennaType() const;
	private:
		LogManager& log_manager_;                         // Logger for debugging and info
		const GnssConfig& config_;                        // Configuration settings
		GnssCorrections gnss_corrections_;                // GNSS correction models
		std::vector<AntennaData> antennas_;               // Antenna correction data
		// removed PppModel& ppp_model_;
		const RinexParser& rinex_parser_;                 // RINEX data parser
		ProductParser& product_parser_;                   // Products
		PppUtils ppp_utils_;		                      // PPP utilities
		
		void loadAntennaData();  // Private loader (calls parseAtx)

		// Interpolates PCV table for antenna corrections
		double interpolatePcv(const std::vector<std::vector<double>>& pcv_table, double zenith,
			double azimuth, double dazi, double dzen) const;

	};

}

#endif