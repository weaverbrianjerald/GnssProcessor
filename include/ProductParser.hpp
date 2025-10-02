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
#include "PppTypes.hpp"

namespace GnssProcessor {

    class ProductParser {
    public:
        ProductParser(const GnssConfig& config, LogManager& log_manager);

        // Retrieve product file paths
        std::vector<std::filesystem::path> getOrbitPaths() const;
        std::filesystem::path getProductPath(const std::string& type) const;

        // Parse external product files
        bool parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits);
        bool parseClk(const std::filesystem::path& path, std::vector<ClockData>& clocks);
        bool parseAtx(const std::filesystem::path& path, std::vector<AntennaData>& antennas);
        bool parseBia(const std::filesystem::path& path, std::vector<BiasData>& biases);
        bool parseErp(const std::filesystem::path& path, std::vector<ErpData>& erps);
        //bool parseOceanTide(const std::filesystem::path& path, std::vector<OceanTideData>& tides);
        bool parseKlobuchar(const std::filesystem::path& path, std::vector<double>& klobuchar_coeffs);

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
    };

} // namespace GnssProcessor