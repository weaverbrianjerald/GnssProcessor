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

/// @file ProductParser.hpp
/// @brief Product file loader and parsers (SP3, CLK, ATX, BIA, ERP, ionosphere).
/// @details Provides utilities to locate product files defined in configuration
/// and parse them into internal product data structures used by the processing pipeline.

namespace GnssProcessor {

    class RinexParser; ///< Forward declaration to break circular ctor dependency

    /**
     * @brief Parser and loader for external GNSS product files.
     *
     * ProductParser locates product files via the provided @c GnssConfig and
     * provides parsing routines for common GNSS auxiliary products:
     * - Orbit (SP3)
     * - Satellite clocks (CLK)
     * - Antenna models (ATX)
     * - Differential biases (BIA)
     * - Earth rotation parameters (ERP)
     * - Klobuchar ionospheric coefficients
     *
     * The class stores parsed product data in member containers which can be
     * accessed through const accessors. A pointer to a @c RinexParser can be
     * supplied after construction using @c setRinexParser to avoid circular
     * dependencies at construction time.
     */
    class ProductParser {
    public:
        /**
         * @brief Construct a ProductParser.
         * @param config Configuration providing product paths and processing options.
         * @param log_manager Log manager used for logging parsing and load operations.
         */
        ProductParser(const GnssConfig& config, LogManager& log_manager);

        /**
         * @brief Allow supplying a RinexParser after construction.
         * @param rinex Reference to an existing @c RinexParser used elsewhere in the application.
         * @note This setter breaks the circular dependency between ProductParser and RinexParser.
         */
        void setRinexParser(RinexParser& rinex);

        /**
         * @brief Get list of orbit product file paths defined in configuration.
         * @return Vector of filesystem paths to orbit (SP3) products.
         */
        std::vector<std::filesystem::path> getOrbitPaths() const;

        /**
         * @brief Get list of clock product file paths defined in configuration.
         * @return Vector of filesystem paths to clock (CLK) products.
         */
        std::vector<std::filesystem::path> getClockPaths() const;

        /**
         * @brief Get product file path for a specific product type.
         * @param type Product type string (e.g., "SP3", "CLK", "ATX").
         * @return Path to product file for the requested type. If not present, may be empty.
         */
        std::filesystem::path getProductPath(const std::string& type) const;

        /**
         * @brief Parse SP3 orbit files into OrbitData entries.
         * @param sp3_paths Paths to SP3 files to parse.
         * @param orbits Output vector populated with parsed @ref OrbitData entries.
         * @return True on success, false on failure (parsing/logging performed via LogManager).
         */
        bool parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits);

        /**
         * @brief Parse satellite clock (CLK) files into ClockData entries.
         *
         * Reads one or more CLK files and merges the results, discarding duplicate
         * epochs (same satellite and time within 1 s).  This supports multi-day
         * processing where clock products span day boundaries.
         *
         * @param clk_paths Paths to CLK files to parse.
         * @param clocks Output vector populated with parsed @ref ClockData entries.
         * @return True on success, false on failure.
         */
        bool parseClk(const std::vector<std::filesystem::path>& clk_paths, std::vector<ClockData>& clocks);

        /**
         * @brief Parse antenna (ATX) file.
         * @param path Path to the ATX file.
         * @param antennas Output vector populated with parsed @ref AntennaData entries.
         * @return True on success, false on failure.
         */
        bool parseAtx(const std::filesystem::path& path, std::vector<AntennaData>& antennas);

        /**
         * @brief Parse bias (BIA) file.
         * @param path Path to the BIA file.
         * @param biases Output vector populated with parsed @ref BiasData entries.
         * @return True on success, false on failure.
         */
        bool parseBia(const std::filesystem::path& path, std::vector<BiasData>& biases);

        /**
         * @brief Parse Earth rotation parameter (ERP) file.
         * @param path Path to the ERP file.
         * @param erps Output vector populated with parsed @ref ErpData entries.
         * @return True on success, false on failure.
         */
        bool parseErp(const std::filesystem::path& path, std::vector<ErpData>& erps);

        /**
         * @brief Parse Klobuchar ionospheric coefficients file.
         * @param path Path to the Klobuchar coefficient file.
         * @param klobuchar_coeffs Output vector populated with coefficients (size typically 4).
         * @return True on success, false on failure.
         */
        bool parseKlobuchar(const std::filesystem::path& path, std::vector<double>& klobuchar_coeffs);

        /**
         * @brief Load all configured products (SP3, CLK, ATX, BIA, ERP, ionosphere).
         * @details This is a convenience method that queries @c GnssConfig for product
         * paths and attempts to parse each required product. Parsing errors are logged
         * via the configured @c LogManager.
         */
        void loadProducts();

        /**
         * @brief Get parsed satellite clock entries.
         * @return Const reference to the internal vector of @ref ClockData.
         */
        const std::vector<ClockData>& getClocks() const { return clocks_; }

        /**
         * @brief Get parsed ERP entries.
         * @return Const reference to the internal vector of @ref ErpData.
         */
        const std::vector<ErpData>& getErps() const { return erps_; }

        /**
         * @brief Get parsed SP3 orbit entries.
         * @return Const reference to the internal vector of @ref OrbitData.
         */
        const std::vector<OrbitData>& getOrbits() const { return orbits_; }

        /**
         * @brief Get parsed antenna model entries.
         * @return Const reference to the internal vector of @ref AntennaData.
         */
        const std::vector<AntennaData>& getAntennas() const { return antennas_; }

        /**
         * @brief Get parsed bias entries.
         * @return Const reference to the internal vector of @ref BiasData.
         */
        const std::vector<BiasData>& getBiases() const { return biases_; }

        /**
         * @brief Get parsed Klobuchar ionospheric coefficients.
         * @return Const reference to the internal vector of double coefficients.
         */
        const std::vector<double>& getKlobucharCoeffs() const { return klobuchar_coeffs_; }


        /**
        * @brief Move orbits out of parser. Parser's orbits_ will be empty after call.
        */
        std::vector<OrbitData> moveOrbits() { return std::move(orbits_); }

        /**
         * @brief Move clocks out of parser. Parser's clocks_ will be empty after call.
         */
        std::vector<ClockData> moveClocks() { return std::move(clocks_); }

        /**
         * @brief Move antennas out of parser. Parser's antennas_ will be empty after call.
         */
        std::vector<AntennaData> moveAntennas() { return std::move(antennas_); }

        /**
         * @brief Move biases out of parser. Parser's biases_ will be empty after call.
         */
        std::vector<BiasData> moveBiases() { return std::move(biases_); }

        /**
         * @brief Move ERPs out of parser. Parser's erps_ will be empty after call.
         */
        std::vector<ErpData> moveErps() { return std::move(erps_); }

        /**
         * @brief Move Klobuchar coefficients out of parser.
         */
        std::vector<double> moveKlobucharCoeffs() { return std::move(klobuchar_coeffs_); }

        /**
         * @brief Move Sun ephemeris out of parser.
         */
        std::vector<SunEphemeris> moveSunEphemeris() { return std::move(sun_ephemeris_); }

        /**
         * @brief Default destructor.
         */
        ~ProductParser() = default;

    private:
        const GnssConfig& config_;         ///< Reference to configuration (not owned).
        LogManager& log_manager_;          ///< Reference to log manager (not owned).
        RinexParser* rinex_parser_ = nullptr; ///< Optional pointer to RinexParser (set via setRinexParser).
        Planetary planetary_;              ///< Planetary utilities (sun/moon models).

        std::vector<OrbitData> orbits_;    ///< Parsed satellite orbit data (SP3).
        std::vector<ClockData> clocks_;    ///< Parsed satellite clock data (CLK).
        std::vector<AntennaData> antennas_;///< Parsed antenna corrections (ATX).
        std::vector<BiasData> biases_;     ///< Parsed differential bias data (BIA).
        std::vector<ErpData> erps_;        ///< Parsed Earth rotation parameters (ERP).
        std::vector<double> klobuchar_coeffs_; ///< Parsed Klobuchar ionospheric coefficients.
        std::vector<SunEphemeris> sun_ephemeris_; ///< Computed or parsed Sun ephemeris for epochs.
    };

} // namespace GnssProcessor