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
 * Given a processing date (data_download.year / data_download.doy), this class
 * follows the strategy of the MATLAB downloadMGEX / downloadRINEX tools:
 *
 *  1. Derive the required date range: observation day ± 1 day for orbits and
 *     clocks, the observation day for biases, broadcast navigation and the GIM,
 *     and a backward day search for the ERP.
 *  2. Compute the expected file names from the IGS long-name conventions.
 *  3. Check the local data root for files already downloaded and decompressed.
 *  4. Download missing files from CDDIS and decompress them.
 *  5. Build the product list and observation path that GnssConfig / ProductParser expect.
 *
 * ## Directory layout (under data_root, date-first):
 * @code
 *   data_root/
 *     <YYYY-DOY>/products/        ready SP3, CLK, BIA, ERP, INX, broadcast nav
 *     <YYYY-DOY>/obs/<station>/   ready observation RINEX
 *     static/atx/   static/nc/    local ATX and ocean-loading files (not downloaded)
 *     cache/<YYYY-DOY>/           raw downloads (.gz, .crx)
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
        int year{ 0 };                      ///< Observation year. 0 = unset; resolve() fails while it is 0.
        int doy{ 0 };                       ///< Observation day-of-year. 0 = unset; resolve() fails while it is 0.
        bool download_rinex{ false };       ///< Whether to download observation RINEX from CDDIS.
        bool download_products{ true };     ///< Whether to download SP3/CLK/BIA/ERP/BRDC.
        bool download_orbits{ true };       ///< Gates the SP3 download only. False in real time when a live orbit source supplies orbits (RTB-4).
        bool download_clocks{ true };       ///< Gates the CLK download only, independent of orbits so that "ULT orbits + network clocks" can be expressed. False in real time when a live or network clock source supplies clocks. BIA/ERP/BRDC download regardless (RTB-4).
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
        std::vector<std::filesystem::path> product_paths;      ///< All product file paths (SP3, CLK, BIA, ERP, ATX, NC, and the IONEX GIM when requested).
    };

    // ------------------------------------------------------------------
    // Ultra-rapid (ULT) orbit naming. Public statics, so unit tests can
    // pin the name grammar, the start-hour walk-back sequence and the
    // per-analysis-centre sampling token without network access.
    // ------------------------------------------------------------------

    /**
     * @brief One candidate start epoch for an ultra-rapid (ULT) orbit file.
     *
     * IGS-convention ultra-rapids are 02D (48 h) products released every
     * 6 h with file-name start hours 00/06/12/18 UTC.
     *
     * Span-convention assumption (the candidate ordering depends on it; also
     * stated at @ref resolveUltOrbits): the 48 h span is 24 h observed
     * followed by 24 h predicted; the file-name epoch @c YYYYDDDHHMM is the
     * start of the observed half, and the file is released about 27 h after
     * that epoch (3 h of processing latency after the observed half ends).
     * The (D, 00) file is therefore the one whose observed half covers the
     * whole processing day D, and each 6 h step back in start epoch moves
     * day-D coverage from fully observed toward fully predicted.
     */
    struct UltStartEpoch {
        int year{ 0 };   ///< Calendar year of the filename epoch.
        int doy{ 0 };    ///< Day-of-year of the filename epoch.
        int hour{ 0 };   ///< Filename start hour: 0, 6, 12 or 18.
    };

    /**
     * @brief Number of start-epoch candidates tried per ULT walk-back.
     *
     * Six candidates in 6 h steps span 30 h of start epochs, which covers the
     * worst-case live gap: with release ≈ start + 27 h, the newest published
     * file's start epoch can be up to about 33 h before the current time (just
     * before the next release). For a session in the 00:00–03:00 UTC window of
     * day D that file is (D-2, 18), the sixth candidate.
     */
    static constexpr int kUltWalkBackTries = 6;

    /**
     * @brief Per-analysis-centre sampling token for ULT orbit long names.
     *
     * CODE publishes 5-minute ultra-rapids (@c 05M); the IGS combined
     * ultra-rapid is 15-minute (@c 15M). Unknown centres default to the
     * IGS-combined @c 15M. When that guess is wrong (a 404), the resolver's
     * alternate-token fallback finds the file at the cost of one extra request.
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
     * is 6 h earlier, crossing day and year boundaries as needed. The
     * obs-day selection anchors at (D, 00), whose observed half covers the
     * whole processing day; the previous-day margin selection anchors at
     * (D-1, 18), the newest file whose start precedes D 00:00.
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
     * Files that cannot be obtained are logged as warnings and left out of
     * @p resolved; they do not make the call fail.
     *
     * @param[out] resolved Paths to the resolved files.
     * @return False only when downloads are enabled and data_root is empty or
     *         year / doy is 0; true otherwise, including when downloads are disabled.
     */
    bool resolve(ResolvedPaths& resolved);

private:
    // --- Internal helpers ---

    /** @brief Create static/atx, static/nc and cache under data_root. The
     *  per-date directories are created when a file is first stored there. */
    void ensureDirectories();

    /**
     * @brief Build CDDIS product URL for a given product tag and YYYYDOY.
     * @param type_override When non-empty, use this product-latency code
     *        (FIN/RAP/ULT) instead of @c config_.product_type. Used to fetch a
     *        rapid ERP in a live real-time session whose orbit product is final.
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
     * Replaces the generic prev/obs/next @c resolveProduct fetch, which does
     * not suit a 48 h product: the three 02D files overlap, and because SP3
     * epoch dedup keeps the first file in parse order, the previous-day file's
     * predicted half would replace the obs-day file's observed half without
     * any message (and the next-day file is redundant).
     *
     * Fetch set, in parse order (the return order is preserved through to
     * @c ProductParser::parseSp3):
     *  1. The obs-day file, selected by the start-hour walk-back
     *     (@ref ultStartCandidates anchored at (D, 00)): its observed half
     *     covers the whole day; its predicted half provides the
     *     post-midnight interpolation margin.
     *  2. One previous-day margin file (walk-back anchored at (D-1, 18))
     *     for the pre-midnight interpolation margin, skipped when the
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
     * a final ERP for the current day because it does not exist yet. Without an
     * ERP, @c erps_ stays empty, the pole tide (up to ~25 mm radial) is dropped
     * without an error, and the Sun/Moon EOP frame degrades. In that case this
     * function uses the short-latency rapid product and walks backward day by
     * day to the most recent available ERP; polar motion / UT1 drift over a few
     * days is far below the pole-tide error budget once flat-extrapolated to the
     * session epoch. Returns an empty path if none resolves within
     * @c kErpMaxAgeDays (7 days). A ULT product_type also takes the rapid ERP.
     *
     * The live-session predicate is @c !download_clocks alone: a session
     * that skips the precise CLK is live whether or not it also pins
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
