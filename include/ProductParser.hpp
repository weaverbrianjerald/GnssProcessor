#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <netcdf.h>
#include <Eigen/Dense>
#include "GnssConfig.hpp"
#include "LogManager.hpp"
#include "RinexParser.hpp" // for GnssTime, System
#include "Types.hpp"

namespace GnssProcessor {

    struct OrbitData {
        std::string satellite;
        GnssTime time;
        Eigen::Vector3d position;
        Eigen::Vector3d velocity;
        bool position_valid;
        bool velocity_valid;
    };

    struct ClockData {
        std::string satellite;
        GnssTime time;
        GnssTime start_time;
        GnssTime end_time;
        double correction;
    };

    struct AntennaData {
        std::string type;
        std::map<std::string, Eigen::Vector3d> pco;
        std::map<std::string, std::vector<std::vector<double>>> pcv;
    };

    struct BiasData {
        std::string satellite;
        std::string signal;
        GnssTime time;
        GnssTime start_time;
        GnssTime end_time;
        double bias;
    };

    struct ErpData {
        GnssTime time;
        double ut1_utc;
        double xpole, ypole;
    };

    struct OceanTideData {
        GnssTime time;
        Eigen::Vector3d displacement;
    };

    class ProductParser {
    public:
        ProductParser(const GnssConfig& config, LogManager& log_manager);
        bool parseSp3(const std::filesystem::path& path, std::vector<OrbitData>& orbits);
        bool parseClk(const std::filesystem::path& path, std::vector<ClockData>& clocks);
        bool parseAtx(const std::filesystem::path& path, std::vector<AntennaData>& antennas);
        bool parseBia(const std::filesystem::path& path, std::vector<BiasData>& biases);
        bool parseErp(const std::filesystem::path& path, std::vector<ErpData>& erps);
        //bool parseOceanTide(const std::filesystem::path& path, std::vector<OceanTideData>& tides);
        bool parseKlobuchar(const std::filesystem::path& path, std::vector<double>& coeffs);
        void parseSunEphemeris(const std::filesystem::path& path, std::vector<SunEphemeris>& sun_ephemeris);

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
    };

} // namespace GnssProcessor

//#endif // PRODUCTPARSER_HPP