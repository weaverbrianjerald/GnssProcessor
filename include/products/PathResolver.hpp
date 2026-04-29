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
 * ## Modes
 *
 *  - @c DataSource::Internal: file discovery is performed by globbing
 *    the local directory tree at @c GnssConfig::getInternalDataRoot().
 *    No network access.  Suited to unit tests, demos, and any run
 *    against the bundled fixtures under @c <project_root>/data.
 *
 *  - @c DataSource::Production: the resolver delegates to
 *    @c ProductResolver, which builds IGS long-name URLs from
 *    @c data_download.year / @c doy / @c station / @c product_source
 *    and downloads anything missing into @c data_download.data_root.
 *    The on-disk layout produced by @c ProductResolver is identical
 *    to the internal tree, so the glob logic used for internal mode
 *    works against a populated production cache too.
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
 * @c setNavigationPath / @c setProductPaths setters plus the new
 * @c setBaselineReferenceRinexPath / @c setBaselineRoverRinexPath
 * setters added for baseline-mode integration.
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
     * @brief Find a RINEX observation file that mentions @p station_name in its filename.
     *
     * Matches IGS long names (@c SSSS00CCC_R_…) and 4-char short names
     * (@c SSSSDDDH.YYO).  Case-insensitive on the station tag.  When
     * multiple candidates exist the first hit in lex order wins.
     * Each entry in @p search_roots is scanned recursively, so
     * per-experiment subdirectories (@c absolute/,
     * @c baseline/short/static/, @c baseline/short/igs_pasadena/)
     * and the production-cache layout (@c products/rinex/unzip/)
     * all work without further configuration.
     *
     * When the input is a 9-char IGS long-name (SSSS00CCC), the
     * 4-char station-ID prefix is also accepted as a match — this is
     * how RINEX-2 short-name files (e.g. @c cit11290.24o) are matched
     * against a configured @c "CIT100USA".
     *
     * @return The matched file, or an empty path if none found.
     */
    std::filesystem::path findObservationByStation(
        const std::vector<std::filesystem::path>& search_roots,
        const std::string& station_name) const;

    const GnssConfig& config_;
    LogManager& log_manager_;
};

} // namespace GnssProcessor
