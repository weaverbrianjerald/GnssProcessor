/**
 * @file ProductResolver.cpp
 * @brief Implements automatic GNSS product and RINEX data resolution and download.
 *
 * Mirrors the MATLAB downloadMGEX / downloadRINEX strategy:
 *  - Compute date range (obs day ± 1 for orbits, obs day for clocks/bias/erp).
 *  - Build IGS long-name filenames using the configured analysis centre prefix
 *    and product type (FIN/RAP/ULT).
 *  - Check local cache; download from CDDIS and decompress if missing.
 *
 * On-disk layout (date-first, since the production-mode rewrite):
 * @code
 *   data_root/
 *     <YYYY-DOY>/                      e.g. "2024-129"
 *       products/                      ready-to-use, no .gz / .crx leftovers
 *         *.SP3   *.CLK   *.BIA   *.ERP   *_MN.rnx (broadcast nav)
 *       obs/<station>/                 per-station observation RINEX
 *         *.rnx
 *     static/                          date-independent static products
 *       atx/igs20.atx
 *       nc/EOT20_load.nc
 *     cache/<YYYY-DOY>/                raw downloads — kept until manually purged
 *       *.gz, *.crx, *.crx.gz, etc.
 * @endcode
 *
 * The "ready" tree under each YYYY-DOY/ directory is what gets pushed to
 * GnssConfig and consumed by ProductParser. The cache/ tree is only for
 * intermediate artifacts (compressed downloads pre-decompression, Hatanaka
 * .crx pre-conversion). A failed crx2rnx leaves the .crx in cache/ so the
 * user can debug or retry without re-downloading.
 */

#include "products/ProductResolver.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProductResolver::ProductResolver(const DownloadConfig& dl_config,
                                 LogManager& log_manager)
    : config_(dl_config),
      log_manager_(log_manager),
      downloader_(log_manager, dl_config.netrc_path,
                  dl_config.keep_zip_after_unzip) {
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------

namespace {
    /// "YYYY-DOY" form, e.g. "2024-129" — the per-date directory name used
    /// throughout the production layout. Matches the standard IGS DOY-based
    /// archive convention while remaining human-readable in a shell prompt.
    std::string dateDirName(int year, int doy) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%03d", year, doy);
        return std::string(buf);
    }

    /// Lowercased copy of a string, ASCII-only.
    std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
} // anonymous namespace

void ProductResolver::ensureDirectories() {
    // Top-level static/ (.atx, .nc) and cache/ (raw downloads) — created lazily
    // here so a fresh data_root is fully populated. The per-date YYYY-DOY/
    // directories are created on demand inside resolveProduct / resolveBrdc /
    // resolveRinex (we don't know the date set until resolve() runs).
    std::filesystem::create_directories(config_.data_root / "static" / "atx");
    std::filesystem::create_directories(config_.data_root / "static" / "nc");
    std::filesystem::create_directories(config_.data_root / "cache");
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

std::string ProductResolver::typeSeg(const std::string& product_type) {
    // Map product_type to the long-name convention segment.
    // FIN -> MGXFIN, RAP -> OPSRAP, ULT -> OPSULT (default FIN).
    if (product_type == "RAP") return "OPSRAP";
    if (product_type == "ULT") return "OPSULT";
    return "MGXFIN";
}

std::string ProductResolver::buildProductFilename(const std::string& tag,
                                                  const std::string& yyyydoy,
                                                  const std::string& type_override) const {
    const std::string& src = config_.product_source; // e.g. "COD"
    const std::string  typ =                         // e.g. "FIN"
        type_override.empty() ? config_.product_type : type_override;

    std::string type_seg = typeSeg(typ);

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
    } else if (tag == "gim") {
        // CODE's GIM uses the OPS campaign segment (not MGX): COD0OPSFIN / COD0OPSRAP.
        const std::string gim_lat = (typ == "RAP") ? "RAP" : "FIN";
        return src + "0OPS" + gim_lat + "_" + yyyydoy + "0000_01D_01H_GIM.INX.gz";
    }
    return {};
}

std::string ProductResolver::buildProductUrl(const std::string& tag,
                                             const std::string& yyyydoy,
                                             int gps_week,
                                             const std::string& type_override) const {
    const std::string host = "https://cddis.nasa.gov";
    std::string filename = buildProductFilename(tag, yyyydoy, type_override);

    if (tag == "brdc") {
        // Broadcast nav is under /archive/gnss/data/daily/YYYY/brdc/
        std::string year_str = yyyydoy.substr(0, 4);
        return host + "/archive/gnss/data/daily/" + year_str + "/brdc/" + filename;
    }
    if (tag == "gim") {
        // IONEX is archived under /products/ionosphere/YYYY/DOY/ (year + day-of-year).
        std::string year_str = yyyydoy.substr(0, 4);
        std::string doy_str  = yyyydoy.substr(4, 3);
        return host + "/archive/gnss/products/ionosphere/" + year_str + "/" + doy_str + "/" + filename;
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
    const std::vector<int>& gps_weeks,
    const std::string& type_override) {

    auto log = log_manager_.getLogger("resolver");
    std::vector<std::filesystem::path> resolved;

    // Effective product-latency code for this resolve (override wins).
    const std::string eff_type =
        type_override.empty() ? config_.product_type : type_override;

    if (tag != "orb" && tag != "clk" && tag != "dcb" && tag != "erp" && tag != "brdc" && tag != "gim") {
        log->error("Unknown product tag: {}", tag);
        return {};
    }

    for (size_t i = 0; i < yyyydoy_list.size(); ++i) {
        const auto& yyyydoy = yyyydoy_list[i];
        int gps_week = (i < gps_weeks.size()) ? gps_weeks[i] : 0;

        // Per-date layout: YYYYDOY → "YYYY-DOY"
        const int year = std::stoi(yyyydoy.substr(0, 4));
        const int doy  = std::stoi(yyyydoy.substr(4, 3));
        const std::string ddir = dateDirName(year, doy);
        const std::filesystem::path ready_dir = config_.data_root / ddir / "products";
        const std::filesystem::path cache_dir = config_.data_root / "cache" / ddir;

        // Filename of the ready (decompressed) artifact.
        std::string filename_gz = buildProductFilename(tag, yyyydoy, type_override);
        std::string filename = filename_gz;
        if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".gz")
            filename = filename.substr(0, filename.size() - 3);

        const std::filesystem::path ready_path = ready_dir / filename;
        if (std::filesystem::exists(ready_path)) {
            log->info("Cached {}: {}", tag, ready_path.string());
            resolved.push_back(ready_path);
            continue;
        }

        // Download .gz to cache/, decompress straight to the ready/ dir.
        std::string url = buildProductUrl(tag, yyyydoy, gps_week, type_override);
        auto result = downloader_.downloadAndDecompress(url, cache_dir, ready_dir);
        if (!result.empty()) {
            resolved.push_back(result);
            continue;
        }

        // For orbits, try 15M fallback if 5M failed (typically a 404).
        if (tag == "orb") {
            std::string type_seg = typeSeg(eff_type);

            std::string fallback_filename =
                config_.product_source + "0" + type_seg + "_" + yyyydoy + "0000_01D_15M_ORB.SP3.gz";
            std::string fallback_url = "https://cddis.nasa.gov/archive/gnss/products/"
                + std::to_string(gps_week) + "/" + fallback_filename;

            log->info("Trying 15M orbit fallback for {}", yyyydoy);
            auto fb_result = downloader_.downloadAndDecompress(fallback_url, cache_dir, ready_dir);
            if (!fb_result.empty()) {
                resolved.push_back(fb_result);
                continue;
            }
        }

        // For ERP, try 01D period fallback if 12H failed.
        if (tag == "erp") {
            std::string type_seg = typeSeg(eff_type);

            std::string fallback_filename =
                config_.product_source + "0" + type_seg + "_" + yyyydoy + "0000_01D_01D_ERP.ERP.gz";
            std::string fallback_url = "https://cddis.nasa.gov/archive/gnss/products/"
                + std::to_string(gps_week) + "/" + fallback_filename;

            log->info("Trying 01D ERP fallback for {}", yyyydoy);
            auto fb_result = downloader_.downloadAndDecompress(fallback_url, cache_dir, ready_dir);
            if (!fb_result.empty()) {
                resolved.push_back(fb_result);
                continue;
            }
        }

        log->warn("Failed to obtain {} for {}", tag, yyyydoy);
    }
    return resolved;
}

// ---------------------------------------------------------------------------
// ERP (Earth-rotation) — real-time-aware latency + backward-date search
// ---------------------------------------------------------------------------

std::pair<int, int> ProductResolver::shiftDate(int year, int doy, int delta) {
    auto isLeap = [](int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
    int y = year;
    int d = doy + delta;
    while (d < 1)                          { y -= 1; d += isLeap(y) ? 366 : 365; }
    for (int diy; d > (diy = isLeap(y) ? 366 : 365); ) { d -= diy; y += 1; }
    return { y, d };
}

std::filesystem::path ProductResolver::resolveErp(int year, int doy) {
    auto log = log_manager_.getLogger("resolver");

    // ERP latency selection. A live real-time session (no precise orbit/clock
    // download) cannot obtain a FINAL ERP for the current day — it does not
    // exist yet — so prefer the short-latency RAPID product, whose polar motion
    // / UT1 is current to within the pole-tide error budget. An explicitly-set
    // RAP/ULT product_type is honoured as-is.
    std::string erp_type = config_.product_type;
    if (!config_.download_orbits_clocks && erp_type == "FIN") {
        erp_type = "RAP";
        log->info("Real-time live source: using RAPID ERP (FINAL not yet "
                  "available for the current day)");
    }

    // Walk backward from the session day to the most recent available ERP.
    // Polar motion (~a few mas/day) and UT1-UTC (~ms/day) drift slowly, so an
    // ERP a few days old — flat-extrapolated to the session epoch by
    // Planetary::interpolateErp — biases the pole tide by well under a mm,
    // versus omitting the ~25 mm-radial correction entirely when erps_ is empty.
    constexpr int kErpMaxAgeDays = 7;
    for (int back = 0; back <= kErpMaxAgeDays; ++back) {
        auto [y, d] = shiftDate(year, doy, -back);
        const std::string yyyydoy = std::to_string(y) + padDoy(d);
        const int week = gpsWeekFromDate(y, d);
        auto paths = resolveProduct("erp", { yyyydoy }, { week }, erp_type);
        if (!paths.empty()) {
            if (back > 0)
                log->info("ERP: current-day product unavailable; using "
                          "{}-day-old {} ERP ({})", back, erp_type, yyyydoy);
            return paths.front();
        }
    }
    log->warn("No {} ERP resolved within {} days of {}{} — pole tide and the "
              "Sun/Moon EOP frame will be degraded",
              erp_type, kErpMaxAgeDays, year, padDoy(doy));
    return {};
}

// ---------------------------------------------------------------------------
// Broadcast navigation
// ---------------------------------------------------------------------------

std::filesystem::path ProductResolver::resolveBrdc(const std::string& yyyydoy) {
    const int year = std::stoi(yyyydoy.substr(0, 4));
    const int doy  = std::stoi(yyyydoy.substr(4, 3));
    const std::string ddir = dateDirName(year, doy);
    const std::filesystem::path ready_dir = config_.data_root / ddir / "products";
    const std::filesystem::path cache_dir = config_.data_root / "cache" / ddir;

    int gps_week = gpsWeekFromDate(year, doy);
    std::string url = buildProductUrl("brdc", yyyydoy, gps_week);
    return downloader_.downloadAndDecompress(url, cache_dir, ready_dir);
}

// ---------------------------------------------------------------------------
// Observation RINEX
//
// The download flow is split into two distinct phases:
//   1. Hatanaka phase: try .crx.gz at each rate. Each rate's failure mode is
//      tracked separately:
//        - Download failure (404, auth, network) → silently try next rate.
//        - Download success but crx2rnx failed   → hard error, do NOT mask
//          by trying .rnx.gz. The user has a misconfigured crx2rnx; falling
//          through would hide that.
//   2. Plain RINEX phase: only entered if every Hatanaka rate failed at the
//      DOWNLOAD step (not the conversion step). Tries .rnx.gz at each rate.
// ---------------------------------------------------------------------------

std::filesystem::path ProductResolver::resolveRinex(const std::string& yyyydoy, int year) {
    auto log = log_manager_.getLogger("resolver");

    if (config_.station.empty()) {
        log->warn("No station configured for RINEX download");
        return {};
    }

    const int doy = std::stoi(yyyydoy.substr(4, 3));
    const std::string ddir = dateDirName(year, doy);

    // Build the 9-char station name: SSSS00CCC (e.g. GCGO00USA)
    std::string station_upper = config_.station;
    std::transform(station_upper.begin(), station_upper.end(), station_upper.begin(), ::toupper);
    std::string long_station;
    if (station_upper.size() == 4) {
        long_station = station_upper + "00" + config_.station_country;
    } else {
        long_station = station_upper; // already 9-char
    }

    // Per-station ready dir; cache/ holds raw downloads + intermediate .crx.
    const std::filesystem::path ready_dir = config_.data_root / ddir / "obs" / toLower(long_station);
    const std::filesystem::path cache_dir = config_.data_root / "cache" / ddir;

    // CDDIS observation archive: /archive/gnss/data/daily/{YYYY}/{DOY}/{YY}d/
    std::string yy = std::to_string(year).substr(2);
    std::string doy_str = yyyydoy.substr(4);
    const std::string cddis_dir =
        "https://cddis.nasa.gov/archive/gnss/data/daily/"
        + std::to_string(year) + "/" + doy_str + "/" + yy + "d/";

    const std::vector<std::string> rate_list = { "30S", "15S", "01S" };

    // Phase 0: short-circuit if a converted .rnx for any rate is
    // already sitting in ready_dir from a prior run.  Without this
    // every re-run re-invokes crx2rnx and re-stages the output, which
    // is wasted work and noisy in the log.  We probe both Hatanaka
    // and plain naming because either may have produced the ready
    // file on the original run.
    {
        std::error_code ec;
        if (std::filesystem::exists(ready_dir, ec)) {
            for (const auto& rate : rate_list) {
                const std::string ready_name =
                    long_station + "_R_" + yyyydoy + "0000_01D_" + rate + "_MO.rnx";
                const std::filesystem::path ready_path = ready_dir / ready_name;
                if (std::filesystem::exists(ready_path, ec)) {
                    log->info("RINEX: cached at {}", ready_path.string());
                    return ready_path;
                }
            }
        }
    }

    // Phase 1: Hatanaka .crx.gz. Convert via crx2rnx after gunzip.
    bool crx_conversion_failure = false;
    for (const auto& rate : rate_list) {
        std::string filename = long_station + "_R_" + yyyydoy + "0000_01D_" + rate + "_MO.crx.gz";
        std::string url = cddis_dir + filename;

        // Decompress into the cache (intermediate .crx); convert into ready_dir.
        // We pass cache_dir as both dirs to downloadAndDecompress because the
        // .crx is an intermediate, not a ready artifact.
        auto crx_path = downloader_.downloadAndDecompress(url, cache_dir, cache_dir);
        if (crx_path.empty()) {
            // Download failed (404 most common — try next rate).
            continue;
        }

        // crx2rnx writes the .rnx next to the .crx by convention. Convert in-
        // place inside the cache, then move the .rnx to the ready_dir.
        auto rnx_in_cache = downloader_.convertCrxToRnx(crx_path, config_.crx2rnx_path);
        if (rnx_in_cache.empty()) {
            // crx2rnx itself failed. The downloader has already logged the
            // exit code + captured stderr. We treat this as a hard error
            // for this run — falling through to .rnx.gz would mask the
            // underlying crx2rnx setup problem. Set a sticky flag and
            // bail out of the Hatanaka phase.
            log->error("RINEX: crx2rnx conversion failed for {} (see downloader log for details)",
                       crx_path.filename().string());
            crx_conversion_failure = true;
            break;
        }

        // Move the converted .rnx into the ready dir.  Windows
        // std::filesystem::rename fails when the target exists; the
        // copy_file fallback uses overwrite_existing so re-runs after a
        // partial stage still succeed.
        std::filesystem::create_directories(ready_dir);
        std::filesystem::path final_path = ready_dir / rnx_in_cache.filename();
        if (final_path != rnx_in_cache) {
            std::error_code ec;
            std::filesystem::rename(rnx_in_cache, final_path, ec);
            if (ec) {
                std::filesystem::copy_file(rnx_in_cache, final_path,
                    std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    log->error("RINEX: failed to stage {} into {}: {}",
                               rnx_in_cache.string(), final_path.string(), ec.message());
                    return {};
                }
                std::filesystem::remove(rnx_in_cache);
            }
        }
        log->info("RINEX: ready at {}", final_path.string());
        return final_path;
    }

    if (crx_conversion_failure) {
        log->error("RINEX: aborting — Hatanaka conversion failed for {} on {}; "
                   "fix crx2rnx (data_download.crx2rnx_path) and retry",
                   config_.station, yyyydoy);
        return {};
    }

    // Phase 2: plain .rnx.gz (some stations only publish uncompressed).
    log->info("RINEX: no Hatanaka file found for {}; trying plain .rnx.gz", config_.station);
    for (const auto& rate : rate_list) {
        std::string filename = long_station + "_R_" + yyyydoy + "0000_01D_" + rate + "_MO.rnx.gz";
        std::string url = cddis_dir + filename;
        auto result = downloader_.downloadAndDecompress(url, cache_dir, ready_dir);
        if (!result.empty()) {
            log->info("RINEX: ready at {}", result.string());
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
    log->info("Resolving data for {} (year={}, doy={}); layout root: {}",
              yyyydoy_obs, year, doy, config_.data_root.string());

    // --- Orbit date range: day-1, day, day+1 ---
    auto makeDateEntry = [](int y, int d) -> std::pair<std::string, int> {
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

    std::vector<std::string> orb_dates  = { yd_prev, yd_obs, yd_next };
    std::vector<int>         orb_weeks  = { wk_prev, wk_obs, wk_next };
    std::vector<std::string> clk_dates  = { yd_prev, yd_obs, yd_next };
    std::vector<int>         clk_weeks  = { wk_prev, wk_obs, wk_next };
    std::vector<std::string> day_dates  = { yd_obs };
    std::vector<int>         day_weeks  = { wk_obs };

    // --- Resolve products ---
    if (config_.download_products) {
        // Orbits/clocks are gated separately: a live real-time session sources
        // them from broadcast+SSR, so skip the SP3/CLK fetch while still pulling
        // the obs-day BIA/ERP/BRDC the engine needs.
        std::vector<std::filesystem::path> orb_paths, clk_paths;
        if (config_.download_orbits_clocks) {
            orb_paths = resolveProduct("orb", orb_dates, orb_weeks);
            clk_paths = resolveProduct("clk", clk_dates, clk_weeks);
        } else {
            log->info("Real-time live source: skipping orbit/clock download");
        }
        auto bia_paths = resolveProduct("dcb", day_dates, day_weeks);
        // ERP: real-time-aware (RAPID + most-recent-available) so a live "today"
        // session does not silently lose the pole tide / EOP frame.
        auto erp_path  = resolveErp(year, doy);

        for (auto& p : orb_paths) resolved.product_paths.push_back(std::move(p));
        for (auto& p : clk_paths) resolved.product_paths.push_back(std::move(p));
        for (auto& p : bia_paths) resolved.product_paths.push_back(std::move(p));
        if (!erp_path.empty()) resolved.product_paths.push_back(std::move(erp_path));

        // Broadcast navigation
        auto brdc_path = resolveBrdc(yd_obs);
        if (!brdc_path.empty()) {
            resolved.navigation_path = brdc_path;
        } else {
            log->warn("Broadcast navigation file not obtained for {}", yd_obs);
        }

        // ATX (static, not downloaded). Lives in static/atx/ by convention but
        // the user can point atx_path anywhere — we trust whatever they set.
        if (!config_.atx_path.empty() && std::filesystem::exists(config_.atx_path)) {
            resolved.product_paths.push_back(config_.atx_path);
        }

        // NC ocean loading (static, not downloaded).
        if (!config_.nc_path.empty() && std::filesystem::exists(config_.nc_path)) {
            resolved.product_paths.push_back(config_.nc_path);
        }

        log->info("Resolved {} product files", resolved.product_paths.size());
    }

    // --- Resolve the IONEX GIM (gated separately from download_products) ---
    if (config_.download_gim) {
        auto gim_paths = resolveProduct("gim", day_dates, day_weeks);
        const bool got = !gim_paths.empty();
        for (auto& p : gim_paths) resolved.product_paths.push_back(std::move(p));
        if (got) log->info("Resolved IONEX GIM for {}", yd_obs);
        else     log->warn("IONEX GIM not obtained for {}", yd_obs);
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
