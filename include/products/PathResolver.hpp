#pragma once

/**
 * @file PathResolver.hpp
 * @brief Single, mode-aware entry point for locating every input file
 *        the processing pipeline needs.
 *
 * @c PathResolver collapses what used to be two parallel and
 * inconsistent code paths (ProductResolver auto-download vs ad-hoc
 * paths in JSON + hard-coded fallback strings) into one class with
 * one method.  Callers do not need to know which mode is active; they
 * receive a populated @c ResolvedPaths struct and feed it back into
 * @c GnssConfig.
 *
 * ## Unified on-disk layout
 *
 * Both internal and production modes use the same date-first tree.
 * The two modes differ in *origin* of the files (bundled fixtures vs
 * automatic downloads) but read from the same shape:
 *
 * @code
 *   <root>/
 *     <YYYY-DOY>/                e.g. "2024-129"
 *       products/                ready-to-use IGS products
 *         *.SP3 *.CLK *.BIA *.ERP *_MN.rnx (broadcast nav)
 *       obs/<station>/           per-station observation RINEX
 *         *.rnx                  (Hatanaka files converted via crx2rnx)
 *     static/                    date-independent assets
 *       atx/igs20.atx
 *       nc/EOT20_load.nc
 *     cache/<YYYY-DOY>/          (production-mode only) raw .gz / .crx
 * @endcode
 *
 * @c <root> is @c GnssConfig::getInternalDataRoot() in internal mode
 * and @c data_download.data_root in production mode — the user can
 * point them at the same directory or keep them separate.  The layout
 * itself is identical, so a directory populated by internal-mode
 * bootstrap is a perfectly valid production-mode cache and vice
 * versa.
 *
 * ## Modes
 *
 *  - @c DataSource::Internal: file discovery is performed by globbing
 *    the local directory tree at @c GnssConfig::getInternalDataRoot().
 *    No network access.  Suited to unit tests, demos, and any run
 *    against the bundled fixtures.
 *
 *  - @c DataSource::Production: the resolver delegates to
 *    @c ProductResolver, which builds IGS long-name URLs from
 *    @c data_download.year / @c doy / @c station / @c product_source
 *    and downloads anything missing into @c data_download.data_root.
 *
 * ## Date selection (internal mode)
 *
 * Internal mode needs to know which @c <YYYY-DOY>/ subdir to read.
 * Sources, in order of preference:
 *  1. The date encoded in @c files.observation_path's filename
 *     (or any baseline @c rinex_path, when in baseline mode).
 *  2. @c data_download.year / @c data_download.doy — these double as
 *     a generic date hint, valid for either mode.
 *  3. The earliest @c <YYYY-DOY>/ subdir under @c <root> (lex sort).
 *
 * ## Path precedence
 *
 *  1. Explicit overrides in the JSON config win — if the user set
 *     @c files.observation_path, @c files.navigation_path, or
 *     @c baseline.{reference,rovers}.rinex_path to a non-empty
 *     existing path, the resolver leaves those alone.
 *  2. Otherwise, the resolver fills the path in by directory scan
 *     (internal) or by download/cache lookup (production).
 *
 * The application driver applies the resolved paths back into
 * @c GnssConfig via the existing @c setObservationPath /
 * @c setNavigationPath / @c setProductPaths setters plus the
 * @c setBaselineReferenceRinexPath / @c setBaselineRoverRinexPath
 * setters used for baseline-mode integration.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class PathResolver
 * @brief Mode-aware locator for RINEX and GNSS product files.
 */
class PathResolver {
public:
    /**
     * @brief Output of a resolve() call.
     *
     * Empty fields mean "no value found / nothing to inject".  Callers
     * are expected to merge non-empty entries back into the
     * @c GnssConfig instance they passed in.
     */
    struct ResolvedPaths {
        std::filesystem::path observation_path;                       ///< PPP / PPP-RTK observation RINEX (single-receiver modes only).
        std::filesystem::path navigation_path;                        ///< Broadcast navigation RINEX.
        std::vector<std::filesystem::path> product_paths;             ///< SP3 / CLK / BIA / ERP / ATX / NC.
        std::filesystem::path baseline_reference_rinex_path;          ///< Reference station RINEX (baseline modes).
        std::vector<std::filesystem::path> baseline_rover_rinex_paths;///< Rover RINEX paths in the same order as @c BaselineConfig::rovers.
    };

    /**
     * @param config Loaded GnssConfig — read-only here; the resolver
     *               does not mutate state.  The caller is responsible
     *               for applying the returned paths back into config.
     * @param log_manager Logging facility.
     */
    PathResolver(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Resolve every required input file for the current mode.
     *
     * On @c true the @p resolved struct is populated with whatever was
     * located.  Empty fields indicate the caller should keep its
     * existing value (or treat the file as missing).  On @c false a
     * non-recoverable error occurred (e.g. the user requested
     * production mode but the resolver failed before it could even
     * try a download).
     *
     * @param[out] resolved Populated paths.
     * @return true on success.
     */
    bool resolve(ResolvedPaths& resolved);

private:
    // ---------------- Internal-mode helpers ----------------

    /** @brief Implements the internal-data scan.  See class doc. */
    bool resolveInternal(ResolvedPaths& resolved);

    /** @brief Implements the production download flow.  Delegates to ProductResolver. */
    bool resolveProduction(ResolvedPaths& resolved);

    /**
     * @brief Glob a directory for files whose extension (case-insensitive)
     *        matches any in @p extensions.
     *
     * Non-recursive.  Returns paths sorted lexicographically so the
     * order is deterministic across runs and platforms.  Missing
     * directories yield an empty vector (not an error).
     *
     * @param dir Directory to scan.
     * @param extensions Extensions to match (with leading dot, lower-case).
     * @return Matching paths.
     */
    std::vector<std::filesystem::path> globByExtension(
        const std::filesystem::path& dir,
        const std::vector<std::string>& extensions) const;

    /**
     * @brief Pick a RINEX observation file for @p station_name from
     *        @c <obs_root>/<station>/.
     *
     * Used by internal-mode resolution to map a configured station
     * identifier to the corresponding obs file inside the date-first
     * tree.  Scans direct children of @p obs_root for a directory
     * whose name (case-insensitive) contains @p station_name; in the
     * matching directory returns the first @c .rnx (lex sort) or, as
     * a fallback, a RINEX-2 @c .NNo short-name file.
     *
     * When the input is a 9-char IGS long-name (SSSS00CCC), the
     * 4-char station-ID prefix is also accepted — this lets a
     * configured @c "CIT100USA" match a directory named @c "cit1/".
     *
     * @return The matched file, or an empty path if none found.
     */
    std::filesystem::path pickStationRnx(
        const std::filesystem::path& obs_root,
        const std::string& station_name) const;

    const GnssConfig& config_;
    LogManager& log_manager_;
};

} // namespace GnssProcessor
