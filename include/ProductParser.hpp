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
#include "Planetary.hpp"

namespace GnssProcessor {

    class RinexParser; // forward declaration to break circular ctor dependency

    class ProductParser {
    public:
        // Constructor
        ProductParser(const GnssConfig& config, LogManager& log_manager);
        //ProductParser(const GnssConfig& config, const RinexParser& rinex, LogManager& log_manager); // keep existing

        // Allow supplying RinexParser later to break circular dependency
        void setRinexParser(RinexParser& rinex);

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

        // Loads SP3, CLK, ATX, BIA, ERP, and ionospheric data
        void loadProducts();

        // Accessors for parsed product data (added to support Geometry/GnssCorrections)
        const std::vector<ClockData>& getClocks() const { return clocks_; }
        const std::vector<ErpData>& getErps() const { return erps_; }
        const std::vector<OrbitData>& getOrbits() const { return orbits_; }
        const std::vector<AntennaData>& getAntennas() const { return antennas_; }
        const std::vector<BiasData>& getBiases() const { return biases_; }
        const std::vector<double>& getKlobucharCoeffs() const { return klobuchar_coeffs_; }

        // Destructor
        ~ProductParser() = default;

    private:
        const GnssConfig& config_;
        LogManager& log_manager_;
        //const RinexParser& rinex_parser_;                 // RINEX data parser
        RinexParser* rinex_parser_ = nullptr;             // pointer, set via setRinexParser
        Planetary planetary_;                             // Planetary (sun and moon) models

        std::vector<OrbitData> orbits_;                   // Satellite orbit data
        std::vector<ClockData> clocks_;                   // Satellite clock data
        std::vector<AntennaData> antennas_;               // Antenna correction data
        std::vector<BiasData> biases_;                    // Bias correction data
        std::vector<ErpData> erps_;                       // Earth rotation parameters
        std::vector<double> klobuchar_coeffs_;            // Klobuchar ionospheric model coefficients
        std::vector<SunEphemeris> sun_ephemeris_;         // Sun ephemeris data
    };

} // namespace GnssProcessor