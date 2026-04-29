#pragma once

#include "products/DataDownloader.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Resolves, downloads and caches GNSS product and RINEX data files.
 *
 * Given a processing date (derived from the observation file or config), this
 * class mirrors the strategy used by the MATLAB downloadMGEX / downloadRINEX
 * tools:
 *
 *  1. Derive the required date range (observation day ± 1 day for orbits).
 *  2. Compute expected filenames following IGS long-name conventions.
 *  3. Check a local data root for previously downloaded/decompressed files.
 *  4. If missing, download from CDDIS and decompress.
 *  5. Build the product list and observation path that GnssConfig / ProductParser expect.
 *
 * ## Directory layout (under data_root):
 * @code
 *   data_root/
 *     orbit/zip/     orbit/unzip/
 *     clock/zip/     clock/unzip/
 *     dcb/zip/       dcb/unzip/
 *     erp/zip/       erp/unzip/
 *     brdc/zip/      brdc/unzip/
 *     rinex/zip/     rinex/unzip/
 *     atx/
 *     nc/
 * @endcode
 */
class ProductResolver {
public:
    /**
     * @brief Configuration block for data download and caching.
     *
     * Populated from the JSON "data_download" section.
     */
    struct DownloadConfig {
        bool enabled{ false };              ///< Master enable for auto-download.
        std::filesystem::path data_root;    ///< Local cache root directory.
        std::string product_source{ "COD" };///< 3-char analysis centre prefix (e.g. "COD", "GFZ").
        std::string product_type{ "FIN" };  ///< Product latency: FIN, RAP, ULT.
        std::filesystem::path netrc_path;   ///< Path to .netrc (empty = default).
        std::string station;                ///< 4- or 9-char station name for RINEX download.
        std::string station_country{ "USA" };///< 3-char country code (for IGS long name).
        int year{ 0 };                      ///< Observation year (0 = auto-detect).
        int doy{ 0 };                       ///< Observation day-of-year (0 = auto-detect).
        bool download_rinex{ false };       ///< Whether to download observation RINEX from CDDIS.
        bool download_products{ true };     ///< Whether to download SP3/CLK/BIA/ERP/BRDC.
        std::filesystem::path atx_path;     ///< Path to a local ATX file (not downloaded).
        std::filesystem::path nc_path;      ///< Path to a local ocean-loading NC file (not downloaded).
        std::filesystem::path crx2rnx_path; ///< Path to crx2rnx executable for Hatanaka decompression.
        bool keep_zip_after_unzip{ false }; ///< Forwarded to DataDownloader; see its doc-comment.
    };

    /**
     * @brief Result of a resolve operation.
     *
     * Contains the paths that should be fed into GnssConfig / ProductParser.
     */
    struct ResolvedPaths {
        std::filesystem::path observation_path;                ///< Observation RINEX.
        std::filesystem::path navigation_path;                 ///< Broadcast navigation RINEX.
        std::vector<std::filesystem::path> product_paths;      ///< All product file paths (SP3, CLK, ATX, BIA, ERP, NC).
    };

    /**
     * @brief Construct a ProductResolver.
     * @param dl_config Download configuration block.
     * @param log_manager Logging facility.
     */
    ProductResolver(const DownloadConfig& dl_config, LogManager& log_manager);

    /**
     * @brief Resolve (and optionally download) all required data files.
     *
     * @param[out] resolved Paths to the resolved files.
     * @return True if all critical files are available, false otherwise.
     */
    bool resolve(ResolvedPaths& resolved);

private:
    // --- Internal helpers ---

    /** @brief Ensure all sub-directories under data_root exist. */
    void ensureDirectories();

    /** @brief Build CDDIS product URL for a given product tag and YYYYDOY. */
    std::string buildProductUrl(const std::string& tag, const std::string& yyyydoy,
                                int gps_week) const;

    /** @brief Build the expected filename for a product tag and YYYYDOY. */
    std::string buildProductFilename(const std::string& tag,
                                     const std::string& yyyydoy) const;

    /** @brief Format an integer DOY as a zero-padded 3-character string. */
    static std::string padDoy(int doy);

    /** @brief Compute GPS week number from year and DOY. */
    static int gpsWeekFromDate(int year, int doy);

    /** @brief Resolve a single product type over a list of YYYYDOY strings. */
    std::vector<std::filesystem::path> resolveProduct(
        const std::string& tag,
        const std::vector<std::string>& yyyydoy_list,
        const std::vector<int>& gps_weeks);

    /** @brief Resolve the broadcast navigation file. */
    std::filesystem::path resolveBrdc(const std::string& yyyydoy);

    /** @brief Resolve observation RINEX from CDDIS. */
    std::filesystem::path resolveRinex(const std::string& yyyydoy, int year);

    DownloadConfig config_;
    LogManager& log_manager_;
    DataDownloader downloader_;
};

} // namespace GnssProcessor
