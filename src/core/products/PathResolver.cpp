/**
 * @file PathResolver.cpp
 * @brief Implementation of the unified internal/production path locator.
 */

#include "products/PathResolver.hpp"
#include "products/ProductResolver.hpp"

#include <algorithm>
#include <cctype>
#include <set>
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
/// otherwise.  Used as a hint to prefer same-day products when
/// multiple are present in the cache.
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

/// Convert a 7-char "YYYYDOY" string to days since 2000-01-01.  Used
/// purely as an ordering / difference primitive — the absolute value
/// is meaningless, only the gap between two values is.  Returns -1 if
/// the input doesn't parse, in which case callers should treat the
/// associated file as date-less.
int yyyydoyToDaysSince2000(const std::string& yyyydoy) {
    if (yyyydoy.size() != 7) return -1;
    int year, doy;
    try {
        year = std::stoi(yyyydoy.substr(0, 4));
        doy  = std::stoi(yyyydoy.substr(4, 3));
    } catch (...) { return -1; }
    if (year < 2000 || year > 2100 || doy < 1 || doy > 366) return -1;
    int days = 0;
    for (int y = 2000; y < year; ++y) {
        const bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
        days += leap ? 366 : 365;
    }
    return days + doy - 1;
}

/// Decide whether a file is in-window for a given date hint.
///
/// "In window" means the file's filename either does not encode a date
/// at all (ATX, EOT20 NC, hand-written test fixtures, etc. — kept
/// unconditionally because they're date-agnostic) OR encodes a date
/// within @p window_days of the hint.  The default ±1 day window
/// matches the orbit/clock interpolation boundary that production-mode
/// ProductResolver uses (prev / obs / next).
///
/// Pre-condition: @p hint is a valid 7-char YYYYDOY string.  Callers
/// that haven't extracted a hint should skip filtering altogether.
bool isProductInDateWindow(const std::filesystem::path& product,
                           const std::string& hint,
                           int window_days = 1) {
    const std::string product_yd = extractYyyyDoy(product);
    if (product_yd.empty()) return true;  // date-less file, always keep
    const int hint_days = yyyydoyToDaysSince2000(hint);
    const int prod_days = yyyydoyToDaysSince2000(product_yd);
    if (hint_days < 0 || prod_days < 0) return true;  // unparseable, err on the side of keeping
    return std::abs(prod_days - hint_days) <= window_days;
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

std::filesystem::path PathResolver::findObservationByStation(
    const std::vector<std::filesystem::path>& search_roots,
    const std::string& station_name) const {

    if (station_name.empty()) return {};

    // Build the list of name fragments to try.  The fixture catalog
    // and most production configs use the IGS 9-char long-name form
    // (SSSS00CCC, e.g. "CIT100USA"), but CDDIS publishes some older
    // stations only as RINEX-2 short names (e.g. "cit11290.24d"),
    // whose filename contains just the 4-char station ID.  When the
    // input looks like a long-name we accept either form.  Pure
    // substring containment is intentionally kept (case-insensitive)
    // so 4-char IDs like "JPLM" still match the long-name file
    // "JPLM00USA_R_..." cleanly.
    std::vector<std::string> needles;
    needles.push_back(station_name);
    if (station_name.size() == 9 &&
        std::isalpha(static_cast<unsigned char>(station_name[0])) &&
        station_name[4] == '0' && station_name[5] == '0') {
        // Looks like SSSS00CCC — also accept the 4-char SSSS prefix.
        needles.push_back(station_name.substr(0, 4));
    }

    // Recursive scan over each search root.  Typical roots:
    //   - <internal_data_root>/observations/      (curated fixtures
    //     under absolute/, baseline/short/...)
    //   - <internal_data_root>/products/rinex/unzip/  (production-mode
    //     download cache and bootstrap intermediate files)
    // Caller controls the list so the same helper works for either
    // PPP (single obs) or baseline (per-station) lookups.
    std::vector<std::filesystem::path> hits;
    for (const auto& root : search_roots) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::exists(root, ec)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string fname = entry.path().filename().string();
            const std::string ext   = toLowerLocal(entry.path().extension().string());
            // Accept .rnx, .rinex, .obs, .NNo (RINEX 2 short-name year-based).
            // .NNd / .NND (Hatanaka-compressed) intentionally NOT accepted
            // here — the RINEX parser can't read them, so handing one back
            // would just defer the failure.  Bootstrap converts to .o
            // before placing in observations/; production-mode
            // ProductResolver does the same for the rinex/unzip cache.
            const bool is_rinex_ext =
                ext == ".rnx" || ext == ".rinex" || ext == ".obs" ||
                (ext.size() == 4 && ext[0] == '.' && std::isdigit(static_cast<unsigned char>(ext[1]))
                                  && std::isdigit(static_cast<unsigned char>(ext[2]))
                                  && (ext[3] == 'o' || ext[3] == 'O'));
            if (!is_rinex_ext) continue;
            for (const auto& needle : needles) {
                if (icontains(fname, needle)) {
                    hits.push_back(entry.path());
                    break;
                }
            }
        }
    }
    std::sort(hits.begin(), hits.end());
    return hits.empty() ? std::filesystem::path{} : hits.front();
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

    const std::filesystem::path products_root     = root / "products";
    const std::filesystem::path observations_root = root / "observations";
    // Search roots for observation RINEX files.  Both the curated
    // observations/ tree (where bootstrap places converted .rnx/.o
    // files) and the production-cache layout (products/rinex/unzip/,
    // where ProductResolver downloads in production mode and where
    // users can drop manually-fetched files) are valid sources.  The
    // resolver picks the first file matching the station name across
    // either root, lex-sorted.
    const std::vector<std::filesystem::path> obs_search_roots = {
        observations_root,
        products_root / "rinex" / "unzip",
    };

    // ---- Products: glob the canonical subdir layout ----------------
    // Matches the directory structure ProductResolver creates in
    // production, so the same files appear regardless of mode.
    auto append = [&](std::vector<std::filesystem::path>&& v) {
        for (auto& p : v) resolved.product_paths.push_back(std::move(p));
    };
    append(globByExtension(products_root / "orbit" / "unzip", {".sp3"}));
    append(globByExtension(products_root / "clock" / "unzip", {".clk"}));
    append(globByExtension(products_root / "dcb"   / "unzip", {".bia", ".bsx"}));
    append(globByExtension(products_root / "erp"   / "unzip", {".erp"}));
    append(globByExtension(products_root / "atx",              {".atx"}));
    append(globByExtension(products_root / "nc",               {".nc"}));
    log->info("Internal mode: discovered {} product files under {}",
        resolved.product_paths.size(), products_root.string());

    // ---- Pick observation files first (they anchor the date) -------
    // PPP / PPP-RTK: scan observations/ for one .rnx / .NNo file.
    // Baseline: per-station matches under observations/ by station name.
    // The "date hint" extracted from the picked obs file is then used
    // to disambiguate when multiple nav / product files are present in
    // the cache (e.g. 2023-321 and 2024-129 both shipped in data/).
    std::string date_hint;

    if (!config_.isBaselineMode() && config_.getObservationPath().empty()) {
        std::vector<std::filesystem::path> obs_candidates;
        for (const auto& search_root : obs_search_roots) {
            std::error_code ec;
            if (search_root.empty() || !std::filesystem::exists(search_root, ec)) continue;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(search_root, ec)) {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                const std::string parent = entry.path().parent_path().filename().string();
                const std::string ext    = toLowerLocal(entry.path().extension().string());
                // Accept .rnx / .rinex / .obs / .NNo (RINEX 2 obs short-name).
                // Hatanaka files are rejected — the parser can't read them.
                const bool is_rinex_ext =
                    ext == ".rnx" || ext == ".rinex" || ext == ".obs" ||
                    (ext.size() == 4 && ext[0] == '.'
                                     && std::isdigit(static_cast<unsigned char>(ext[1]))
                                     && std::isdigit(static_cast<unsigned char>(ext[2]))
                                     && (ext[3] == 'o' || ext[3] == 'O'));
                if (!is_rinex_ext) continue;
                obs_candidates.push_back(entry.path());
                // Prefer files under .../absolute/ — they're the canonical
                // PPP fixtures.  First such hit wins.
                if (resolved.observation_path.empty() && icontains(parent, "absolute")) {
                    resolved.observation_path = entry.path();
                }
            }
        }
        if (resolved.observation_path.empty() && !obs_candidates.empty()) {
            std::sort(obs_candidates.begin(), obs_candidates.end());
            resolved.observation_path = obs_candidates.front();
        }
        if (!resolved.observation_path.empty()) {
            date_hint = extractYyyyDoy(resolved.observation_path);
            log->info("Internal mode: observation = {} (date hint='{}')",
                resolved.observation_path.string(), date_hint);
        } else {
            log->warn("Internal mode: no observation RINEX found under {} or {}",
                observations_root.string(),
                (products_root / "rinex" / "unzip").string());
        }
    } else if (config_.isBaselineMode()) {
        const auto& bc = config_.getBaselineConfig();
        if (bc.reference.rinex_path.empty()) {
            const auto p = findObservationByStation(obs_search_roots, bc.reference.name);
            if (!p.empty()) {
                resolved.baseline_reference_rinex_path = p;
                log->info("Internal mode: baseline reference '{}' = {}",
                    bc.reference.name, p.string());
            } else {
                log->warn("Internal mode: no RINEX matching reference station '{}' "
                          "under {} or {}",
                    bc.reference.name, observations_root.string(),
                    (products_root / "rinex" / "unzip").string());
            }
        }
        resolved.baseline_rover_rinex_paths.resize(bc.rovers.size());
        for (size_t i = 0; i < bc.rovers.size(); ++i) {
            if (!bc.rovers[i].rinex_path.empty()) continue;
            const auto p = findObservationByStation(obs_search_roots, bc.rovers[i].name);
            if (!p.empty()) {
                resolved.baseline_rover_rinex_paths[i] = p;
                log->info("Internal mode: baseline rover '{}' = {}",
                    bc.rovers[i].name, p.string());
            } else {
                log->warn("Internal mode: no RINEX matching rover station '{}' "
                          "under {} or {}",
                    bc.rovers[i].name, observations_root.string(),
                    (products_root / "rinex" / "unzip").string());
            }
        }
        // Prefer the reference station's path for the date hint;
        // fall back to whatever's already on the config (manual override).
        std::filesystem::path date_hint_src = !resolved.baseline_reference_rinex_path.empty()
            ? resolved.baseline_reference_rinex_path
            : bc.reference.rinex_path;
        if (date_hint_src.empty() && !resolved.baseline_rover_rinex_paths.empty()) {
            for (const auto& p : resolved.baseline_rover_rinex_paths) {
                if (!p.empty()) { date_hint_src = p; break; }
            }
        }
        if (!date_hint_src.empty()) date_hint = extractYyyyDoy(date_hint_src);
        if (!date_hint.empty()) log->info("Internal mode: baseline date hint='{}'", date_hint);
    }

    // ---- Pinned observation path: extract date_hint ----------------
    // When the user supplies @c files.observation_path explicitly, the
    // auto-discovery block above is skipped and @c date_hint stays empty.
    // The product-filter at the bottom of this function then becomes a
    // no-op and ProductParser dutifully loads every CLK/SP3/BIA in the
    // cache regardless of date.  Extract the hint here so the filter
    // works in both auto and pinned modes.
    if (date_hint.empty() && !config_.isBaselineMode()
        && !config_.getObservationPath().empty()) {
        date_hint = extractYyyyDoy(config_.getObservationPath());
        log->info("Internal mode: observation pinned to '{}' (date hint='{}')",
            config_.getObservationPath().string(), date_hint);
    }

    // ---- Broadcast navigation (date-aware) ------------------------
    // Prefer products/brdc/unzip (matches ProductResolver layout); fall
    // back to legacy products/rnx/ if some user has hand-curated files
    // there from before this refactor.  Within the candidate set we
    // prefer (a) a filename containing date_hint, then (b) "_MN." for
    // multi-system broadcast nav (covers every constellation).
    if (config_.getNavigationPath().empty()) {
        auto nav_candidates = globByExtension(products_root / "brdc" / "unzip", {".rnx"});
        if (nav_candidates.empty())
            nav_candidates = globByExtension(products_root / "rnx", {".rnx"});
        if (!nav_candidates.empty()) {
            std::filesystem::path picked;
            // Pass 1: date hint + multi-system.
            if (!date_hint.empty()) {
                for (const auto& p : nav_candidates) {
                    const std::string fn = p.filename().string();
                    if (fn.find(date_hint) != std::string::npos &&
                        icontains(fn, "_MN.")) { picked = p; break; }
                }
            }
            // Pass 2: date hint only.
            if (picked.empty() && !date_hint.empty()) {
                for (const auto& p : nav_candidates) {
                    if (p.filename().string().find(date_hint) != std::string::npos) {
                        picked = p; break;
                    }
                }
            }
            // Pass 3: any multi-system file.
            if (picked.empty()) {
                for (const auto& p : nav_candidates) {
                    if (icontains(p.filename().string(), "_MN.")) { picked = p; break; }
                }
            }
            // Pass 4: anything.
            if (picked.empty()) picked = nav_candidates.front();
            resolved.navigation_path = picked;
            log->info("Internal mode: navigation = {}", picked.string());
        } else {
            log->warn("Internal mode: no broadcast navigation file under {} or {}",
                (products_root / "brdc" / "unzip").string(),
                (products_root / "rnx").string());
        }
    }

    // ---- Date-filter the product list ----------------------------
    // The glob pass above happily pulled in every SP3/CLK/BIA/ERP it
    // could find, including products from unrelated fixture dates
    // sitting in the same cache (e.g. 2023-321 and 2024-129 both ship
    // in the bundled data set).  ProductParser would then load them
    // all, which is wasteful and noisy at best and can confuse
    // interpolation at day boundaries at worst.  Now that we have a
    // date hint from the obs file, drop products outside the
    // ±1-day orbit window — the same window ProductResolver uses in
    // production mode.  Date-less files (igs20.atx, EOT20_load.nc)
    // pass through because their filenames don't encode a YYYYDOY.
    if (!date_hint.empty() && !resolved.product_paths.empty()) {
        const size_t before = resolved.product_paths.size();
        std::vector<std::filesystem::path> kept;
        kept.reserve(before);
        std::vector<std::string> dropped_for_log;
        for (auto& p : resolved.product_paths) {
            if (isProductInDateWindow(p, date_hint, /*window_days=*/1)) {
                kept.push_back(std::move(p));
            } else {
                dropped_for_log.push_back(p.filename().string());
            }
        }
        resolved.product_paths = std::move(kept);
        log->info("Internal mode: date-filtered products around hint '{}': "
                  "kept {}/{} (dropped {})",
                  date_hint, resolved.product_paths.size(), before,
                  dropped_for_log.size());
        if (!dropped_for_log.empty() && log->level() == spdlog::level::debug) {
            for (const auto& fn : dropped_for_log) {
                log->debug("  dropped (out of window): {}", fn);
            }
        }
    }

    // Friendly first-run hint.  If we ended up with nothing — no
    // products, no nav, no obs (or no baseline obs in baseline mode) —
    // the most likely cause is a fresh clone that hasn't run the
    // bootstrap yet.  The .gitignore keeps only README.md files
    // under data/, so the directories exist but are empty of fixtures.
    const bool found_anything =
        !resolved.product_paths.empty() ||
        !resolved.navigation_path.empty() ||
        !resolved.observation_path.empty() ||
        !resolved.baseline_reference_rinex_path.empty() ||
        std::any_of(resolved.baseline_rover_rinex_paths.begin(),
                    resolved.baseline_rover_rinex_paths.end(),
                    [](const std::filesystem::path& p) { return !p.empty(); });
    if (!found_anything) {
        log->warn("Internal data tree {} appears unpopulated.", root.string());
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
