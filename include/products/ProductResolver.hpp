#pragma once

#include "products/DataDownloader.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include <filesystem>
#include <string>
#include <utility>
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
        bool download_orbits{ true };       ///< RTB-4: gates SP3 download only. False in real-time when a live orbit source supplies orbits.
        bool download_clocks{ true };       ///< RTB-4: gates CLK download only (independent of orbits so "ULT orbits + network clocks" is expressible). False in real-time when a live/network clock source supplies clocks. BIA/ERP/BRDC still download regardless.
        bool download_gim{ false };         ///< Whether to download the IONEX GIM (global ionosphere map).
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

    // ------------------------------------------------------------------
    // Ultra-rapid (ULT) orbit naming — public statics so unit tests can
    // pin the name grammar, the start-hour walk-back sequence and the
    // per-analysis-centre sampling token without touching the network.
    // ------------------------------------------------------------------

    /**
     * @brief One candidate start epoch for an ultra-rapid (ULT) orbit file.
     *
     * IGS-convention ultra-rapids are 02D (48 h) products released every
     * 6 h with filename start hours 00/06/12/18 UTC.
     *
     * SPAN-CONVENTION ASSUMPTION (load-bearing for the candidate ordering,
     * documented here and at @ref resolveUltOrbits): the 48 h span splits
     * into 24 h OBSERVED followed by 24 h PREDICTED; the filename epoch
     * @c YYYYDDDHHMM is the start of the OBSERVED half, and the file is
     * released roughly 27 h after that epoch (3 h processing latency past
     * the end of the observed half).  Consequently the (D, 00) file is the
     * one whose observed half covers the whole processing day D, and
     * walking the start epoch back in 6 h steps degrades day-D coverage
     * from fully-observed to fully-predicted.
     */
    struct UltStartEpoch {
        int year{ 0 };   ///< Calendar year of the filename epoch.
        int doy{ 0 };    ///< Day-of-year of the filename epoch.
        int hour{ 0 };   ///< Filename start hour: 0, 6, 12 or 18.
    };

    /**
     * @brief Number of start-epoch candidates tried per ULT walk-back.
     *
     * Six 6-h steps span 30 h of start epochs, which covers the worst-case
     * live gap: with release ≈ start + 27 h, the newest published file's
     * start epoch can be up to ~33 h before "now" (just before the next
     * release lands), i.e. as old as (D-2, 18) for a session in the
     * 00:00–03:00 UTC window of day D — the sixth candidate.
     */
    static constexpr int kUltWalkBackTries = 6;

    /**
     * @brief Per-analysis-centre sampling token for ULT orbit long names.
     *
     * CODE publishes 5-minute ultra-rapids (@c 05M), the IGS combined
     * ultra-rapid is 15-minute (@c 15M).  Unknown centres default to the
     * IGS-combined @c 15M; the alternate-token 404 fallback in the resolver
     * self-heals a wrong guess at the cost of one extra request.
     *
     * @param product_source 3-char analysis-centre prefix (e.g. "COD", "IGS").
     * @return Sampling token, "05M" or "15M".
     */
    static std::string ultSamplingToken(const std::string& product_source);

    /**
     * @brief The other member of the {05M, 15M} sampling-token pair.
     * @param token A sampling token ("05M" or "15M").
     * @return "05M" for "15M", otherwise "15M".
     */
    static std::string ultAlternateSamplingToken(const std::string& token);

    /**
     * @brief Build the IGS long-name ULT orbit filename (compressed form).
     *
     * Grammar: @c <SRC>0OPSULT_<YYYYDDDHHMM>_02D_<SMP>_ORB.SP3.gz with
     * @c MM always "00" (ultra-rapids start on the hour).
     *
     * @param product_source 3-char analysis-centre prefix (e.g. "COD").
     * @param year           Calendar year of the filename epoch.
     * @param doy            Day-of-year of the filename epoch.
     * @param start_hour     Start hour (0/6/12/18).
     * @param sampling_token "05M" or "15M" (see @ref ultSamplingToken).
     * @return The .gz filename as published on CDDIS.
     */
    static std::string buildUltOrbFilename(const std::string& product_source,
                                           int year, int doy, int start_hour,
                                           const std::string& sampling_token);

    /**
     * @brief ULT start-epoch candidate sequence: 6-h walk-back from
     *        (@p year, @p doy, @p first_hour).
     *
     * The first candidate is the anchor itself; each subsequent candidate
     * is 6 h earlier, crossing day and year boundaries as needed.  The
     * obs-day selection anchors at (D, 00) — observed half covers the whole
     * processing day — and the previous-day margin selection anchors at
     * (D-1, 18) — the freshest file whose nodes precede D 00:00.
     *
     * @param year       Anchor year.
     * @param doy        Anchor day-of-year.
     * @param first_hour Anchor start hour (0/6/12/18).
     * @param count      Number of candidates (default @ref kUltWalkBackTries).
     * @return Candidates in try-order, newest start epoch first.
     */
    static std::vector<UltStartEpoch> ultStartCandidates(
        int year, int doy, int first_hour, int count = kUltWalkBackTries);

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

    /**
     * @brief Build CDDIS product URL for a given product tag and YYYYDOY.
     * @param type_override When non-empty, use this product-latency code
     *        (FIN/RAP/ULT) instead of @c config_.product_type. Used to fetch a
     *        RAPID ERP in a live real-time session whose orbit product is FINAL.
     */
    std::string buildProductUrl(const std::string& tag, const std::string& yyyydoy,
                                int gps_week, const std::string& type_override = {}) const;

    /** @brief Build the expected filename for a product tag and YYYYDOY.
     *  @param type_override See @c buildProductUrl. */
    std::string buildProductFilename(const std::string& tag,
                                     const std::string& yyyydoy,
                                     const std::string& type_override = {}) const;

    /** @brief Map a product-latency code (FIN/RAP/ULT) to its long-name segment. */
    static std::string typeSeg(const std::string& product_type);

    /** @brief Shift (year, doy) by @p delta days, normalizing across year ends. */
    static std::pair<int, int> shiftDate(int year, int doy, int delta);

    /** @brief Format an integer DOY as a zero-padded 3-character string. */
    static std::string padDoy(int doy);

    /** @brief Compute GPS week number from year and DOY. */
    static int gpsWeekFromDate(int year, int doy);

    /** @brief Resolve a single product type over a list of YYYYDOY strings.
     *  @param type_override See @c buildProductUrl. */
    std::vector<std::filesystem::path> resolveProduct(
        const std::string& tag,
        const std::vector<std::string>& yyyydoy_list,
        const std::vector<int>& gps_weeks,
        const std::string& type_override = {});

    /**
     * @brief Resolve ultra-rapid (ULT) orbit files for a processing day.
     *
     * Dedicated ULT path replacing the generic prev/obs/next @c resolveProduct
     * fetch, which is wrong for a 48 h product: the three 02D files overlap,
     * and because SP3 epoch dedup is first-file-wins in parse order, the
     * prev-day file's PREDICTED half would silently shadow the obs-day file's
     * OBSERVED half (and the next-day file is wholly redundant).
     *
     * Fetch set (in PARSE order — the return order is preserved all the way
     * into @c ProductParser::parseSp3):
     *  1. The obs-day file, selected by the start-hour walk-back
     *     (@ref ultStartCandidates anchored at (D, 00)): its observed half
     *     covers the whole day; its predicted half provides the
     *     post-midnight interpolation margin.
     *  2. One previous-day margin file (walk-back anchored at (D-1, 18))
     *     for the pre-midnight interpolation margin — skipped when the
     *     obs-day selection already walked back to a previous-day start.
     *
     * Each candidate is tried with the per-centre sampling token first and
     * the alternate token second (@ref ultAlternateSamplingToken), for the
     * ready-cache probe as well as the download.
     *
     * @param year Processing-day year.
     * @param doy  Processing-day day-of-year.
     * @return Resolved SP3 paths, obs-day file first; empty when no obs-day
     *         candidate resolves (an error listing every tried name is logged).
     */
    std::vector<std::filesystem::path> resolveUltOrbits(int year, int doy);

    /**
     * @brief Resolve the ERP (Earth-rotation) product, real-time-aware.
     *
     * A live real-time session (broadcast+SSR orbit/clock source) cannot obtain
     * a FINAL ERP for the current day — it does not exist yet — which would
     * leave @c erps_ empty and silently drop the pole tide (up to ~25 mm radial)
     * and degrade the Sun/Moon EOP frame. This prefers the short-latency RAPID
     * product in that case and walks backward day-by-day to the most recent
     * available ERP; polar motion / UT1 drift over a few days is far below the
     * pole-tide error budget once flat-extrapolated to the session epoch.
     * Returns an empty path if none resolves within @c kErpMaxAgeDays.
     *
     * The "live session" predicate is @c !download_clocks alone: a session
     * that skips the precise CLK is live regardless of whether it also pins
     * precise ultra-rapid orbits (RTB-4 @c realtime.use_precise_orbits, which
     * re-arms @c download_orbits while clocks stay on the live/network source).
     */
    std::filesystem::path resolveErp(int year, int doy);

    /** @brief Resolve the broadcast navigation file. */
    std::filesystem::path resolveBrdc(const std::string& yyyydoy);

    /** @brief Resolve observation RINEX from CDDIS. */
    std::filesystem::path resolveRinex(const std::string& yyyydoy, int year);

    DownloadConfig config_;
    LogManager& log_manager_;
    DataDownloader downloader_;
};

} // namespace GnssProcessor
