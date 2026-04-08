/**
 * @file ProductResolver.cpp
 * @brief Implements automatic GNSS product and RINEX data resolution and download.
 *
 * Mirrors the MATLAB downloadMGEX / downloadRINEX strategy:
 *  - Compute date range (obs day ± 1 for orbits, obs day for clocks/bias/erp).
 *  - Build IGS long-name filenames using the configured analysis centre prefix
 *    and product type (FIN/RAP/ULT).
 *  - Check local cache; download from CDDIS and decompress if missing.
 */

#include "ProductResolver.hpp"
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProductResolver::ProductResolver(const DownloadConfig& dl_config,
                                 LogManager& log_manager)
    : config_(dl_config),
      log_manager_(log_manager),
      downloader_(log_manager, dl_config.netrc_path) {
}

// ---------------------------------------------------------------------------
// Directory layout
// ---------------------------------------------------------------------------

void ProductResolver::ensureDirectories() {
    const std::vector<std::string> subdirs = {
        "orbit/zip", "orbit/unzip",
        "clock/zip", "clock/unzip",
        "dcb/zip",   "dcb/unzip",
        "erp/zip",   "erp/unzip",
        "brdc/zip",  "brdc/unzip",
        "rinex/zip", "rinex/unzip",
        "atx", "nc"
    };
    for (const auto& sub : subdirs) {
        std::filesystem::create_directories(config_.data_root / sub);
    }
}

// ---------------------------------------------------------------------------
// Filename / URL construction (IGS long-name conventions)
// ---------------------------------------------------------------------------

std::string ProductResolver::padDoy(int doy) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03d", doy);
    return std::string(buf);
}

int ProductResolver::gpsWeekFromDate(int year, int doy) {
    int month, day;
    TimeUtils::dayOfYearToDate(year, doy, month, day);
    GnssTime gt = TimeUtils::gregorianToGpsTime(year, month, day, 0, 0, 0.0);
    return gt.week;
}

std::string ProductResolver::buildProductFilename(const std::string& tag,
                                                  const std::string& yyyydoy) const {
    const std::string& src = config_.product_source; // e.g. "COD"
    const std::string& typ = config_.product_type;   // e.g. "FIN"

    // Map product_type to the naming convention segment.
    // FIN -> MGXFIN, RAP -> OPSRAP, ULT -> OPSULT
    std::string type_seg;
    if (typ == "FIN")      type_seg = "MGXFIN";
    else if (typ == "RAP") type_seg = "OPSRAP";
    else if (typ == "ULT") type_seg = "OPSULT";
    else                   type_seg = "MGXFIN";

    // PREFIX0TYPE_YYYYDOY0000_01D_RATE_PRODUCT.EXT.gz
    if (tag == "orb") {
        return src + "0" + type_seg + "_" + yyyydoy + "0000_01D_05M_ORB.SP3.gz";
    } else if (tag == "clk") {
        return src + "0" + type_seg + "_" + yyyydoy + "0000_01D_30S_CLK.CLK.gz";
    } else if (tag == "dcb") {
        return src + "0" + type_seg + "_" + yyyydoy + "0000_01D_01D_OSB.BIA.gz";
    } else if (tag == "erp") {
        return src + "0" + type_seg + "_" + yyyydoy + "0000_01D_12H_ERP.ERP.gz";
    } else if (tag == "brdc") {
        return "BRDC00IGS_R_" + yyyydoy + "0000_01D_MN.rnx.gz";
    }
    return {};
}

std::string ProductResolver::buildProductUrl(const std::string& tag,
                                             const std::string& yyyydoy,
                                             int gps_week) const {
    const std::string host = "https://cddis.nasa.gov";
    std::string filename = buildProductFilename(tag, yyyydoy);

    if (tag == "brdc") {
        // Broadcast nav is under /archive/gnss/data/daily/YYYY/brdc/
        std::string year_str = yyyydoy.substr(0, 4);
        return host + "/archive/gnss/data/daily/" + year_str + "/brdc/" + filename;
    }
    // All other products are under /archive/gnss/products/GPSWEEK/
    return host + "/archive/gnss/products/" + std::to_string(gps_week) + "/" + filename;
}

// ---------------------------------------------------------------------------
// Resolve a single product category
// ---------------------------------------------------------------------------

std::vector<std::filesystem::path> ProductResolver::resolveProduct(
    const std::string& tag,
    const std::vector<std::string>& yyyydoy_list,
    const std::vector<int>& gps_weeks) {

    auto log = log_manager_.getLogger("resolver");
    std::vector<std::filesystem::path> resolved;

    // Map tag -> subdirectory name.
    std::string subdir;
    if      (tag == "orb") subdir = "orbit";
    else if (tag == "clk") subdir = "clock";
    else if (tag == "dcb") subdir = "dcb";
    else if (tag == "erp") subdir = "erp";
    else if (tag == "brdc") subdir = "brdc";
    else { log->error("Unknown product tag: {}", tag); return {}; }

    std::filesystem::path zip_dir   = config_.data_root / subdir / "zip";
    std::filesystem::path unzip_dir = config_.data_root / subdir / "unzip";

    for (size_t i = 0; i < yyyydoy_list.size(); ++i) {
        const auto& yyyydoy = yyyydoy_list[i];
        int gps_week = (i < gps_weeks.size()) ? gps_weeks[i] : 0;

        // Check if the unzipped file already exists.
        std::string filename_gz = buildProductFilename(tag, yyyydoy);
        std::string filename = filename_gz;
        if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".gz")
            filename = filename.substr(0, filename.size() - 3);

        std::filesystem::path unzipped = unzip_dir / filename;
        if (std::filesystem::exists(unzipped)) {
            log->info("Cached {}: {}", tag, unzipped.string());
            resolved.push_back(unzipped);
            continue;
        }

        // Attempt download + decompress.
        std::string url = buildProductUrl(tag, yyyydoy, gps_week);
        auto result = downloader_.downloadAndDecompress(url, zip_dir, unzip_dir);
        if (!result.empty()) {
            resolved.push_back(result);
        } else {
            // For orbits, try 15M fallback if 5M failed.
            if (tag == "orb") {
                std::string type_seg;
                if (config_.product_type == "FIN")      type_seg = "MGXFIN";
                else if (config_.product_type == "RAP") type_seg = "OPSRAP";
                else if (config_.product_type == "ULT") type_seg = "OPSULT";
                else                                    type_seg = "MGXFIN";

                std::string fallback_filename =
                    config_.product_source + "0" + type_seg + "_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
                std::string fallback_url = "https://cddis.nasa.gov/archive/gnss/products/"
                    + std::to_string(gps_week) + "/" + fallback_filename;

                log->info("Trying 15M orbit fallback for {}", yyyydoy);
                auto fb_result = downloader_.downloadAndDecompress(fallback_url, zip_dir, unzip_dir);
                if (!fb_result.empty()) {
                    resolved.push_back(fb_result);
                    continue;
                }
            }
            // For ERP, try 01D period fallback if 12H failed.
            if (tag == "erp") {
                std::string type_seg;
                if (config_.product_type == "FIN")      type_seg = "MGXFIN";
                else if (config_.product_type == "RAP") type_seg = "OPSRAP";
                else if (config_.product_type == "ULT") type_seg = "OPSULT";
                else                                    type_seg = "MGXFIN";

                std::string fallback_filename =
                    config_.product_source + "0" + type_seg + "_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
                std::string fallback_url = "https://cddis.nasa.gov/archive/gnss/products/"
                    + std::to_string(gps_week) + "/" + fallback_filename;

                log->info("Trying 01D ERP fallback for {}", yyyydoy);
                auto fb_result = downloader_.downloadAndDecompress(fallback_url, zip_dir, unzip_dir);
                if (!fb_result.empty()) {
                    resolved.push_back(fb_result);
                    continue;
                }
            }
            log->warn("Failed to obtain {} for {}", tag, yyyydoy);
        }
    }
    return resolved;
}

// ---------------------------------------------------------------------------
// Broadcast navigation
// ---------------------------------------------------------------------------

std::filesystem::path ProductResolver::resolveBrdc(const std::string& yyyydoy) {
    std::filesystem::path zip_dir   = config_.data_root / "brdc" / "zip";
    std::filesystem::path unzip_dir = config_.data_root / "brdc" / "unzip";

    std::string year_str = yyyydoy.substr(0, 4);
    int year = std::stoi(year_str);
    std::string doy_str = yyyydoy.substr(4);
    int doy = std::stoi(doy_str);
    int gps_week = gpsWeekFromDate(year, doy);

    std::string url = buildProductUrl("brdc", yyyydoy, gps_week);
    return downloader_.downloadAndDecompress(url, zip_dir, unzip_dir);
}

// ---------------------------------------------------------------------------
// Observation RINEX
// ---------------------------------------------------------------------------

std::filesystem::path ProductResolver::resolveRinex(const std::string& yyyydoy, int year) {
    auto log = log_manager_.getLogger("resolver");

    if (config_.station.empty()) {
        log->warn("No station configured for RINEX download");
        return {};
    }

    std::filesystem::path zip_dir   = config_.data_root / "rinex" / "zip";
    std::filesystem::path unzip_dir = config_.data_root / "rinex" / "unzip";

    // Try standard rate list: 30S, 15S, 01S
    const std::vector<std::string> rate_list = { "30S", "15S", "01S" };
    // Build the 9-char station name: SSSS00CCC (e.g. GCGO00USA)
    std::string station_upper = config_.station;
    std::transform(station_upper.begin(), station_upper.end(), station_upper.begin(), ::toupper);

    std::string long_station;
    if (station_upper.size() == 4) {
        long_station = station_upper + "00" + config_.station_country;
    } else {
        long_station = station_upper; // already 9-char
    }

    // CDDIS stores RINEX 3 observations under a YYd/ subdirectory:
    //   /archive/gnss/data/daily/{YYYY}/{DOY}/{YY}d/{filename}
    std::string yy = std::to_string(year).substr(2); // 2-digit year
    std::string doy_str = yyyydoy.substr(4);

    for (const auto& rate : rate_list) {
        std::string filename = long_station + "_R_" + yyyydoy + "0000_01D_" + rate + "_MO.crx.gz";
        std::string url = "https://cddis.nasa.gov/archive/gnss/data/daily/"
            + std::to_string(year) + "/" + doy_str + "/" + yy + "d/"
            + filename;

        auto result = downloader_.downloadAndDecompress(url, zip_dir, unzip_dir);
        if (!result.empty()) {
            // The decompressed file is Hatanaka-compressed (.crx).
            // Convert to standard RINEX (.rnx) using crx2rnx.
            auto rnx = downloader_.convertCrxToRnx(result, config_.crx2rnx_path);
            if (!rnx.empty()) return rnx;
            log->warn("CRX->RNX conversion failed for {}; trying next rate", result.string());
        }
    }

    // Try the plain .rnx.gz variant (some stations provide uncompressed RINEX)
    for (const auto& rate : rate_list) {
        std::string filename = long_station + "_R_" + yyyydoy + "0000_01D_" + rate + "_MO.rnx.gz";
        std::string url = "https://cddis.nasa.gov/archive/gnss/data/daily/"
            + std::to_string(year) + "/" + doy_str + "/" + yy + "d/"
            + filename;

        auto result = downloader_.downloadAndDecompress(url, zip_dir, unzip_dir);
        if (!result.empty()) {
            return result;
        }
    }

    log->warn("Could not find RINEX for station {} on {}", config_.station, yyyydoy);
    return {};
}

// ---------------------------------------------------------------------------
// Top-level resolve
// ---------------------------------------------------------------------------

bool ProductResolver::resolve(ResolvedPaths& resolved) {
    auto log = log_manager_.getLogger("resolver");

    if (!config_.enabled) {
        log->info("Data download disabled; skipping auto-resolve");
        return true; // not an error; caller uses manually specified paths
    }

    if (config_.data_root.empty()) {
        log->error("data_download.data_root not configured");
        return false;
    }

    ensureDirectories();

    // --- Determine processing date ---
    int year = config_.year;
    int doy  = config_.doy;
    if (year == 0 || doy == 0) {
        log->error("data_download.year and data_download.doy must be specified");
        return false;
    }

    const std::string yyyydoy_obs = std::to_string(year) + padDoy(doy);
    log->info("Resolving data for {} (year={}, doy={})", yyyydoy_obs, year, doy);

    // --- Orbit date range: day-1, day, day+1 ---
    // Compute previous and next day (handling year boundaries).
    auto makeDateEntry = [](int y, int d) -> std::pair<std::string, int> {
        // Adjust for year boundaries.
        bool is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int days_in_year = is_leap ? 366 : 365;
        if (d < 1) { y -= 1; is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)); d = is_leap ? 366 : 365; }
        if (d > days_in_year) { y += 1; d = 1; }
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04d%03d", y, d);
        return { std::string(buf), gpsWeekFromDate(y, d) };
    };

    auto [yd_prev, wk_prev] = makeDateEntry(year, doy - 1);
    auto [yd_obs,  wk_obs]  = makeDateEntry(year, doy);
    auto [yd_next, wk_next] = makeDateEntry(year, doy + 1);

    // Orbit: 3 days (prev, obs, next)
    std::vector<std::string> orb_dates  = { yd_prev, yd_obs, yd_next };
    std::vector<int>         orb_weeks  = { wk_prev, wk_obs, wk_next };

    // Clock: 3 days (same as orbit for interpolation at day boundaries)
    std::vector<std::string> clk_dates  = { yd_prev, yd_obs, yd_next };
    std::vector<int>         clk_weeks  = { wk_prev, wk_obs, wk_next };

    // Bias, ERP: observation day only
    std::vector<std::string> day_dates  = { yd_obs };
    std::vector<int>         day_weeks  = { wk_obs };

    // --- Resolve products ---
    if (config_.download_products) {
        auto orb_paths = resolveProduct("orb", orb_dates, orb_weeks);
        auto clk_paths = resolveProduct("clk", clk_dates, clk_weeks);
        auto bia_paths = resolveProduct("dcb", day_dates, day_weeks);
        auto erp_paths = resolveProduct("erp", day_dates, day_weeks);

        for (auto& p : orb_paths) resolved.product_paths.push_back(std::move(p));
        for (auto& p : clk_paths) resolved.product_paths.push_back(std::move(p));
        for (auto& p : bia_paths) resolved.product_paths.push_back(std::move(p));
        for (auto& p : erp_paths) resolved.product_paths.push_back(std::move(p));

        // Broadcast navigation
        auto brdc_path = resolveBrdc(yd_obs);
        if (!brdc_path.empty()) {
            resolved.navigation_path = brdc_path;
        } else {
            log->warn("Broadcast navigation file not obtained for {}", yd_obs);
        }

        // ATX (static, not downloaded)
        if (!config_.atx_path.empty() && std::filesystem::exists(config_.atx_path)) {
            resolved.product_paths.push_back(config_.atx_path);
        }

        // NC ocean loading (static, not downloaded)
        if (!config_.nc_path.empty() && std::filesystem::exists(config_.nc_path)) {
            resolved.product_paths.push_back(config_.nc_path);
        }

        log->info("Resolved {} product files", resolved.product_paths.size());
    }

    // --- Resolve RINEX observation ---
    if (config_.download_rinex) {
        auto rnx_path = resolveRinex(yd_obs, year);
        if (!rnx_path.empty()) {
            resolved.observation_path = rnx_path;
            log->info("Resolved observation RINEX: {}", rnx_path.string());
        } else {
            log->warn("Observation RINEX not obtained for station {} on {}",
                      config_.station, yd_obs);
        }
    }

    return true;
}

} // namespace GnssProcessor
