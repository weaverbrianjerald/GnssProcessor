/**
 * @file PathResolver.cpp
 * @brief Implementation of the unified internal/production path locator.
 */

#include "products/PathResolver.hpp"
#include "products/ProductResolver.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

namespace {

/// ASCII-only lowercase helper (file-local; the config helper lives
/// in the @c GnssConfig translation unit which we don't include here).
std::string toLowerLocal(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// True if @p haystack contains @p needle, case-insensitive.
bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const std::string h = toLowerLocal(haystack);
    const std::string n = toLowerLocal(needle);
    return h.find(n) != std::string::npos;
}

/// Best-effort date extraction from a RINEX or IGS-product filename.
/// Returns a 7-character "YYYYDOY" string on success, empty string
/// otherwise.  Used as a date hint when the user pins
/// @c observation_path / a baseline @c rinex_path.
///
/// Recognises both IGS long-name variants and RINEX-2 short names:
///
///  - Observation long name (9-char station block):
///      @c SSSSCCCNNN_R_YYYYDOY????_...  → YYYYDOY at offset 12
///  - Product long name (10-char source/type block):
///      @c AAAVPPPTTT_YYYYDOY????_...    → YYYYDOY at offset 11
///  - Broadcast nav long name (9-char station + _R_ data flag):
///      @c BRDC00IGS_R_YYYYDOY????_...   → YYYYDOY at offset 12
///  - RINEX-2 short name (8.3 form):
///      @c SSSSDDDH.YYO / .YYD           → year=2000+YY, doy=DDD
///
/// The long-name path is implemented as a generic scan rather than a
/// fixed offset because the 9-vs-10-char prefix difference between
/// observation/nav and product files would otherwise need a per-file
/// lookup.  The scan picks the first 7-digit substring that is
/// preceded by a non-digit character (so we're at the start of a
/// digit block, not mid-stream) and whose decoded (year, doy) parses
/// as a plausible date.
std::string extractYyyyDoy(const std::filesystem::path& p) {
    const std::string fname = p.filename().string();

    auto allDigits = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    auto isValidYyyyDoy = [&](const std::string& s) {
        if (s.size() != 7 || !allDigits(s)) return false;
        const int year = std::stoi(s.substr(0, 4));
        const int doy  = std::stoi(s.substr(4, 3));
        return year >= 1990 && year <= 2100 && doy >= 1 && doy <= 366;
    };

    // RINEX-2 short name first.  YYYY and DOY straddle the dot
    // separator, so the contiguous-digit scan below would never find
    // them as a single token.
    if (fname.size() >= 12 && fname[8] == '.') {
        const std::string doy_str = fname.substr(4, 3);
        const std::string yy_str  = fname.substr(9, 2);
        if (allDigits(doy_str) && allDigits(yy_str)) {
            // Two-digit year: assume 2000+YY (pre-2000 fixtures aren't expected).
            const int year = 2000 + std::stoi(yy_str);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04d%s", year, doy_str.c_str());
            return std::string(buf);
        }
    }

    // Long-name scan.  Walk the filename, take the first 7-digit
    // window that starts a fresh digit block and parses to a valid
    // year/DOY.  This makes the extractor agnostic to whether the
    // YYYYDOY sits at offset 11 (products: COD0MGXFIN_...) or
    // offset 12 (obs/nav: SSSSCCCNNN_R_...).
    for (size_t i = 0; i + 7 <= fname.size(); ++i) {
        if (i > 0 && std::isdigit(static_cast<unsigned char>(fname[i - 1]))) continue;
        const std::string candidate = fname.substr(i, 7);
        if (isValidYyyyDoy(candidate)) return candidate;
    }
    return {};
}

/// Format (year, doy) as the canonical "YYYY-DOY" date-dir name used
/// throughout the unified layout (e.g. @c "2024-129").
std::string dateDirName(int year, int doy) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%03d", year, doy);
    return std::string(buf);
}

/// True if @p name matches the @c "YYYY-DOY" date-dir convention.
/// Used to enumerate the per-date subdirs under @c <root>.
bool isDateDirName(const std::string& name) {
    if (name.size() != 8 || name[4] != '-') return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    }
    for (size_t i = 5; i < 8; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    }
    return true;
}

/// Shift (@p year, @p doy) by @p delta whole days, handling year
/// rollover and leap years.  @p delta is typically -1 or +1 for the
/// orbit/clock interpolation window.
std::pair<int, int> shiftDate(int year, int doy, int delta) {
    auto isLeap = [](int y) {
        return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    };
    int y = year;
    int d = doy + delta;
    while (d < 1) {
        y -= 1;
        d += isLeap(y) ? 366 : 365;
    }
    while (d > (isLeap(y) ? 366 : 365)) {
        d -= isLeap(y) ? 366 : 365;
        y += 1;
    }
    return { y, d };
}

} // anonymous namespace

// ---------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------

PathResolver::PathResolver(const GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

// ---------------------------------------------------------------------
// Public dispatch
// ---------------------------------------------------------------------

bool PathResolver::resolve(ResolvedPaths& resolved) {
    auto log = log_manager_.getLogger("resolver");
    if (config_.isInternalDataSource()) {
        log->info("PathResolver: internal mode (root={})",
            config_.getInternalDataRoot().string());
        return resolveInternal(resolved);
    }
    log->info("PathResolver: production mode (download root={})",
        config_.getDownloadConfig().data_root.string());
    return resolveProduction(resolved);
}

// ---------------------------------------------------------------------
// Internal-mode helpers
// ---------------------------------------------------------------------

std::vector<std::filesystem::path> PathResolver::globByExtension(
    const std::filesystem::path& dir,
    const std::vector<std::string>& extensions) const {

    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string ext = toLowerLocal(entry.path().extension().string());
        for (const auto& want : extensions) {
            if (ext == want) { out.push_back(entry.path()); break; }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::filesystem::path PathResolver::pickStationRnx(
    const std::filesystem::path& obs_root,
    const std::string& station_name) const {

    std::error_code ec;
    if (station_name.empty()) return {};
    if (!std::filesystem::exists(obs_root, ec)) return {};

    // Build the list of name fragments to try.  The fixture catalog
    // and most production configs use the IGS 9-char long-name form
    // (SSSS00CCC, e.g. "CIT100USA"), but some directories may be
    // named with the 4-char station ID alone (matches lower-case
    // production cache convention) — accept both.
    std::vector<std::string> needles{ station_name };
    if (station_name.size() == 9 &&
        std::isalpha(static_cast<unsigned char>(station_name[0])) &&
        station_name[4] == '0' && station_name[5] == '0') {
        needles.push_back(station_name.substr(0, 4));
    }

    for (const auto& entry : std::filesystem::directory_iterator(obs_root, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const std::string dname = entry.path().filename().string();
        bool matches = false;
        for (const auto& n : needles) {
            if (icontains(dname, n)) { matches = true; break; }
        }
        if (!matches) continue;

        // Prefer .rnx in lex order.
        auto rnxs = globByExtension(entry.path(), {".rnx"});
        if (!rnxs.empty()) return rnxs.front();

        // Fallback: RINEX-2 .NNo short-name files (case-insensitive).
        std::error_code ec2;
        std::vector<std::filesystem::path> short_hits;
        for (const auto& f : std::filesystem::directory_iterator(entry.path(), ec2)) {
            if (ec2) break;
            if (!f.is_regular_file()) continue;
            const std::string ext = toLowerLocal(f.path().extension().string());
            const bool is_rnx2 = ext.size() == 4 && ext[0] == '.'
                && std::isdigit(static_cast<unsigned char>(ext[1]))
                && std::isdigit(static_cast<unsigned char>(ext[2]))
                && (ext[3] == 'o' || ext[3] == 'O');
            if (is_rnx2) short_hits.push_back(f.path());
        }
        std::sort(short_hits.begin(), short_hits.end());
        if (!short_hits.empty()) return short_hits.front();
    }
    return {};
}

bool PathResolver::resolveInternal(ResolvedPaths& resolved) {
    auto log = log_manager_.getLogger("resolver");
    const std::filesystem::path root = config_.getInternalDataRoot();

    if (root.empty() || !std::filesystem::exists(root)) {
        log->error("Internal data root does not exist: {}", root.string());
        log->error("Run: python scripts/bootstrap_internal_data.py "
                   "(from the repo root) to populate it.");
        return false;
    }

    // -------- Step 1: determine the observation date ---------------
    // Three ranked sources: the pinned obs filename, data_download
    // year/doy (which doubles as a generic date hint even in internal
    // mode), then the earliest <YYYY-DOY>/ subdir under <root>.
    int year = 0, doy = 0;
    std::string date_hint;

    if (!config_.getObservationPath().empty()) {
        date_hint = extractYyyyDoy(config_.getObservationPath());
    }
    if (date_hint.empty() && config_.isBaselineMode()) {
        const auto& bc = config_.getBaselineConfig();
        if (!bc.reference.rinex_path.empty()) {
            date_hint = extractYyyyDoy(bc.reference.rinex_path);
        }
        for (size_t i = 0; date_hint.empty() && i < bc.rovers.size(); ++i) {
            if (!bc.rovers[i].rinex_path.empty()) {
                date_hint = extractYyyyDoy(bc.rovers[i].rinex_path);
            }
        }
    }
    if (!date_hint.empty()) {
        year = std::stoi(date_hint.substr(0, 4));
        doy  = std::stoi(date_hint.substr(4, 3));
        log->info("Internal mode: date hint '{}-{:03d}' from pinned RINEX path",
            year, doy);
    }
    if (year == 0 || doy == 0) {
        const auto& dl = config_.getDownloadConfig();
        if (dl.year > 0 && dl.doy > 0) {
            year = dl.year;
            doy  = dl.doy;
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04d%03d", year, doy);
            date_hint = buf;
            log->info("Internal mode: date '{}-{:03d}' from data_download.year/doy",
                year, doy);
        }
    }
    if (year == 0 || doy == 0) {
        // Last resort: walk <root>/ for YYYY-DOY/ subdirs.
        std::vector<std::filesystem::path> date_dirs;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;
            if (isDateDirName(entry.path().filename().string())) {
                date_dirs.push_back(entry.path());
            }
        }
        if (date_dirs.empty()) {
            log->error("Internal mode: no <YYYY-DOY>/ subdir under {} and no "
                       "date hint from observation_path or "
                       "data_download.year/doy",
                       root.string());
            log->error("Run: python scripts/bootstrap_internal_data.py "
                       "to populate the layout.");
            return false;
        }
        std::sort(date_dirs.begin(), date_dirs.end());
        const std::string name = date_dirs.front().filename().string();
        year = std::stoi(name.substr(0, 4));
        doy  = std::stoi(name.substr(5, 3));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04d%03d", year, doy);
        date_hint = buf;
        log->info("Internal mode: auto-detected date {}-{:03d} from {}/",
            year, doy, name);
    }

    // -------- Step 2: discover products by date --------------------
    // Orbit + clock pull from a 3-day window (prev/obs/next) so
    // satellite interpolation has boundary points.  BIA/ERP/BRDC
    // are obs-day only — they're piecewise constant or interpolated
    // across the day boundary differently.
    const auto [yp, dp] = shiftDate(year, doy, -1);
    const auto [yn, dn] = shiftDate(year, doy, +1);

    const std::filesystem::path prev_day_products = root / dateDirName(yp, dp)   / "products";
    const std::filesystem::path obs_day_products  = root / dateDirName(year, doy)/ "products";
    const std::filesystem::path next_day_products = root / dateDirName(yn, dn)   / "products";

    auto append = [&](std::vector<std::filesystem::path>&& v) {
        for (auto& p : v) resolved.product_paths.push_back(std::move(p));
    };

    for (const auto& d : { prev_day_products, obs_day_products, next_day_products }) {
        append(globByExtension(d, {".sp3"}));
        append(globByExtension(d, {".clk"}));
    }
    append(globByExtension(obs_day_products, {".bia", ".bsx"}));
    append(globByExtension(obs_day_products, {".erp"}));

    // Static (date-less) assets.  Live under a fixed @c static/
    // subtree at the root; every date shares them.
    append(globByExtension(root / "static" / "atx", {".atx"}));
    append(globByExtension(root / "static" / "nc",  {".nc"}));

    log->info("Internal mode: discovered {} product files for date {}-{:03d}",
        resolved.product_paths.size(), year, doy);

    // -------- Step 3: broadcast navigation --------------------------
    // Same products dir; pick the *_MN.rnx (multi-system broadcast
    // nav) when present, else any .rnx in there as a fallback.
    if (config_.getNavigationPath().empty()) {
        const auto nav_candidates = globByExtension(obs_day_products, {".rnx"});
        std::filesystem::path picked;
        for (const auto& p : nav_candidates) {
            if (icontains(p.filename().string(), "_MN.")) { picked = p; break; }
        }
        if (picked.empty() && !nav_candidates.empty()) picked = nav_candidates.front();
        if (!picked.empty()) {
            resolved.navigation_path = picked;
            log->info("Internal mode: navigation = {}", picked.string());
        } else {
            log->warn("Internal mode: no broadcast navigation file under {}",
                obs_day_products.string());
        }
    }

    // -------- Step 4: observations ---------------------------------
    // <root>/<YYYY-DOY>/obs/<station>/.  PPP / PPP-RTK pick a single
    // file (preferring the configured station if any); baseline mode
    // resolves per-station against the BaselineConfig entries.
    const std::filesystem::path obs_root = root / dateDirName(year, doy) / "obs";

    if (!config_.isBaselineMode() && config_.getObservationPath().empty()) {
        std::filesystem::path picked;
        const auto& dl = config_.getDownloadConfig();
        if (!dl.station.empty()) {
            picked = pickStationRnx(obs_root, dl.station);
        }
        if (picked.empty()) {
            // No station configured / no match — fall back to the
            // first .rnx under any obs/<station>/ subdir.
            std::error_code ec;
            std::vector<std::filesystem::path> all_obs;
            if (std::filesystem::exists(obs_root, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(obs_root, ec)) {
                    if (ec) break;
                    if (!entry.is_directory()) continue;
                    auto rnxs = globByExtension(entry.path(), {".rnx"});
                    for (auto& p : rnxs) all_obs.push_back(std::move(p));
                }
            }
            std::sort(all_obs.begin(), all_obs.end());
            if (!all_obs.empty()) picked = all_obs.front();
        }
        if (!picked.empty()) {
            resolved.observation_path = picked;
            log->info("Internal mode: observation = {}", picked.string());
        } else {
            log->warn("Internal mode: no observation RINEX under {}",
                obs_root.string());
        }
    } else if (config_.isBaselineMode()) {
        const auto& bc = config_.getBaselineConfig();
        if (bc.reference.rinex_path.empty()) {
            const auto p = pickStationRnx(obs_root, bc.reference.name);
            if (!p.empty()) {
                resolved.baseline_reference_rinex_path = p;
                log->info("Internal mode: baseline reference '{}' = {}",
                    bc.reference.name, p.string());
            } else {
                log->warn("Internal mode: no RINEX matching reference '{}' under {}",
                    bc.reference.name, obs_root.string());
            }
        }
        resolved.baseline_rover_rinex_paths.resize(bc.rovers.size());
        for (size_t i = 0; i < bc.rovers.size(); ++i) {
            if (!bc.rovers[i].rinex_path.empty()) continue;
            const auto p = pickStationRnx(obs_root, bc.rovers[i].name);
            if (!p.empty()) {
                resolved.baseline_rover_rinex_paths[i] = p;
                log->info("Internal mode: baseline rover '{}' = {}",
                    bc.rovers[i].name, p.string());
            } else {
                log->warn("Internal mode: no RINEX matching rover '{}' under {}",
                    bc.rovers[i].name, obs_root.string());
            }
        }
    }

    // -------- Step 5: empty-tree warning ---------------------------
    const bool found_anything =
        !resolved.product_paths.empty() ||
        !resolved.navigation_path.empty() ||
        !resolved.observation_path.empty() ||
        !resolved.baseline_reference_rinex_path.empty() ||
        std::any_of(resolved.baseline_rover_rinex_paths.begin(),
                    resolved.baseline_rover_rinex_paths.end(),
                    [](const std::filesystem::path& p) { return !p.empty(); });
    if (!found_anything) {
        log->warn("Internal data tree {} appears unpopulated for date {}-{:03d}.",
            root.string(), year, doy);
        log->warn("Run: python scripts/bootstrap_internal_data.py "
                  "(from the repo root) to download the bundled fixtures.");
    }

    return true;
}

// ---------------------------------------------------------------------
// Production-mode delegation
// ---------------------------------------------------------------------

bool PathResolver::resolveProduction(ResolvedPaths& resolved) {
    auto log = log_manager_.getLogger("resolver");
    const auto& dl = config_.getDownloadConfig();

    if (!dl.enabled) {
        // Source is Production but downloads are off — that's the
        // legacy "all paths supplied manually" mode.  Nothing to do.
        log->info("Production mode with downloads disabled: trusting "
                  "manually-specified paths in the config");
        return true;
    }

    // Map GnssConfig::DownloadConfig -> ProductResolver::DownloadConfig.
    // The two structs are intentionally separate: GnssConfig stores
    // user input, ProductResolver works in the file-locator domain.
    ProductResolver::DownloadConfig rc;
    rc.enabled           = dl.enabled;
    rc.data_root         = dl.data_root;
    rc.product_source    = dl.product_source;
    rc.product_type      = dl.product_type;
    rc.netrc_path        = dl.netrc_path;
    rc.station           = dl.station;
    rc.station_country   = dl.station_country;
    rc.year              = dl.year;
    rc.doy               = dl.doy;
    rc.download_rinex    = dl.download_rinex;
    rc.download_products = dl.download_products;
    rc.atx_path          = dl.atx_path;
    rc.nc_path           = dl.nc_path;
    rc.crx2rnx_path      = dl.crx2rnx_path;
    rc.keep_zip_after_unzip = dl.keep_zip_after_unzip;

    ProductResolver downloader(rc, log_manager_);
    ProductResolver::ResolvedPaths inner;
    if (!downloader.resolve(inner)) {
        log->error("Production mode: ProductResolver failed");
        return false;
    }

    resolved.observation_path = inner.observation_path;
    resolved.navigation_path  = inner.navigation_path;
    resolved.product_paths    = std::move(inner.product_paths);
    // Baseline-mode RINEX paths: production downloads aren't yet wired
    // for arbitrary-station baseline runs (ProductResolver::resolveRinex
    // uses one configured station only).  Leave the baseline_*_path
    // fields empty so Application::runBaseline() falls back to the
    // explicit JSON paths, which is the expected workflow today.
    return true;
}

} // namespace GnssProcessor
