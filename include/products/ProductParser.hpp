#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <netcdf.h>
#include <Eigen/Dense>
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "rinex/RinexParser.hpp" // for GnssTime, System
#include "common/PppTypes.hpp"

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
     * accessed through const accessors.
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
         * @brief Get the configured orbit product file paths.
         * @return Configured product paths with extension ".SP3" (case-sensitive) that
         *         exist on disk.
         */
        std::vector<std::filesystem::path> getOrbitPaths() const;

        /**
         * @brief Get the configured clock product file paths.
         * @return Configured product paths with extension ".CLK" (case-sensitive) that
         *         exist on disk.
         */
        std::vector<std::filesystem::path> getClockPaths() const;

        /**
         * @brief Get product file path for a specific product type.
         * @param type Product type string (e.g., "ATX", "BIA", "ERP", "RNX").
         * @return Path to the product file for the requested type, or an empty path
         *         (with a warning) when none is configured or the file does not exist.
         */
        std::filesystem::path getProductPath(const std::string& type) const;

        /**
         * @brief Parse SP3 orbit files into OrbitData entries.
         *
         * Positions are converted from km to m and velocities from dm/s to m/s.  Epochs
         * are read as GPS time.  Records with an invalid position are dropped; across
         * files, an epoch within 1 s of one already stored for that satellite is skipped
         * (the first file read wins).  This is the only source of the orbit list.
         *
         * @param sp3_paths Paths to SP3 files to parse.
         * @param orbits Output vector populated with parsed @ref OrbitData entries.
         * @return True when at least one orbit entry was parsed, false otherwise
         *         (errors are logged via LogManager).
         */
        bool parseSp3(const std::vector<std::filesystem::path>& sp3_paths, std::vector<OrbitData>& orbits);

        /**
         * @brief Parse satellite clock (CLK) files into ClockData entries.
         *
         * Reads the "AS" (satellite) records of one or more CLK files and merges the
         * results, discarding duplicate epochs (same satellite and time within 1 s;
         * the first file read wins).  This supports multi-day processing where clock
         * products span day boundaries.
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
         * @brief Parse a bias (BIA, SINEX-BIAS) file.
         * @param path Path to the BIA file.
         * @param biases Output vector populated with the satellite OSB records as
         *               @ref BiasData entries (bias and std-dev converted from ns to m).
         *               Receiver records go to @ref getReceiverBiases.
         * @param clock_refs Output map populated with per-system clock reference signal pairs
         *                   parsed from the BIAS/DESCRIPTION block (may be empty if not present).
         * @return True when at least one satellite or receiver record was parsed, false otherwise.
         */
        bool parseBia(const std::filesystem::path& path, std::vector<BiasData>& biases,
                      ClockRefMap& clock_refs);

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
         * @param klobuchar_coeffs Output vector: the 8 GPS coefficients (alpha0..alpha3,
         *        then beta0..beta3) read from the RINEX navigation header; cleared on failure.
         * @return True when exactly 8 coefficients were read, false otherwise.
         */
        bool parseKlobuchar(const std::filesystem::path& path, std::vector<double>& klobuchar_coeffs);

        /**
         * @brief Load all configured products (SP3, CLK, ATX, BIA, ERP, ionosphere).
         * @details This is a convenience method that queries @c GnssConfig for product
         * paths and attempts to parse each required product. Parsing errors are logged
         * via the configured @c LogManager.
         *
         * @param load_precise_orbits When true (default), precise SP3 orbit products
         *        are parsed into the cache.  Pass false in real-time when a live
         *        broadcast+SSR orbit source replaces them (see
         *        @c GnssConfig::realtimeUsesLiveOrbitSource).
         * @param load_precise_clocks When true (default), precise CLK clock products
         *        are parsed into the cache.  Pass false in real-time when a
         *        live/network clock source replaces them (see
         *        @c GnssConfig::realtimeUsesLiveOrbitClockSource).  The two flags are
         *        separate so a config can load precise (ultra-rapid) orbits while
         *        taking clocks live ("ULT orbits + network clocks") (RTB-4).  An empty
         *        cache makes the corresponding @c Geometry / @c GnssCorrections query
         *        resolve through the live store instead of a stale fixture.  The
         *        remaining products (ATX, BIA, ERP, and Klobuchar from the navigation
         *        file) are always loaded.
         */
        void loadProducts(bool load_precise_orbits = true, bool load_precise_clocks = true);

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
         * @brief Get parsed receiver bias entries (item 0b of the EXT-CB ladder).
         *
         * OSB and DSB records from the BIAS/SOLUTION block whose STATION field is
         * non-empty.  Satellite entries never appear here and receiver entries never
         * appear in @ref getBiases, so consumers of the satellite vector are
         * unaffected.  Populated by @ref parseBia (appended per call, never cleared);
         * empty before the first call, and a satellite-only file adds nothing.  No production
         * code consumes these yet; estimating receiver biases is separate work.
         */
        const std::vector<ReceiverBiasData>& getReceiverBiases() const { return receiver_biases_; }

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
         * @brief Move clock reference signal map out of parser.
         */
        ClockRefMap moveClockRefSignals() { return std::move(clock_refs_); }

        /**
         * @brief Move ERPs out of parser. Parser's erps_ will be empty after call.
         */
        std::vector<ErpData> moveErps() { return std::move(erps_); }

        /**
         * @brief Move Klobuchar coefficients out of parser.
         */
        std::vector<double> moveKlobucharCoeffs() { return std::move(klobuchar_coeffs_); }

        /**
         * @brief Default destructor.
         */
        ~ProductParser() = default;

    private:
        const GnssConfig& config_;         ///< Reference to configuration (not owned).
        LogManager& log_manager_;          ///< Reference to log manager (not owned).

        std::vector<OrbitData> orbits_;    ///< Parsed satellite orbit data (SP3).
        std::vector<ClockData> clocks_;    ///< Parsed satellite clock data (CLK).
        std::vector<AntennaData> antennas_;///< Parsed antenna corrections (ATX).
        std::vector<BiasData> biases_;     ///< Parsed differential bias data (BIA).
        std::vector<ReceiverBiasData> receiver_biases_;  ///< Parsed receiver bias records (item 0b; stored, not yet consumed).
        ClockRefMap clock_refs_;              ///< Per-system satellite clock reference observables from BIA DESCRIPTION.
        std::vector<ErpData> erps_;        ///< Parsed Earth rotation parameters (ERP).
        std::vector<double> klobuchar_coeffs_; ///< Parsed Klobuchar ionospheric coefficients.

        /** @brief Filter configured product paths by file extension. */
        std::vector<std::filesystem::path> getPathsByExtension(const std::string& ext) const;

        /** @brief Join a vector of paths into a comma-separated string. */
        static std::string joinPaths(const std::vector<std::filesystem::path>& paths);

        /** @brief Validate a 3-char satellite PRN (system letter + 2 digits). */
        static bool isValidPrn(const std::string& prn);

        /** @brief Right-trim whitespace from a string in-place. */
        static void rtrim(std::string& s);

        /** @brief Remove all whitespace from a string. */
        static std::string sanitize(const std::string& s);
    };

} // namespace GnssProcessor