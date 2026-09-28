#pragma once

#include "logging/LogManager.hpp"
#include <filesystem>
#include <string>

namespace GnssProcessor {

/**
 * @brief Low-level HTTP download and .gz decompression utilities.
 *
 * Uses libcurl for HTTPS transfers and zlib for gzip decompression.
 * CDDIS authentication uses the standard .netrc mechanism (libcurl reads the
 * file when instructed to).
 *
 * The .netrc path is resolved once, on the first download() call:
 *   1. If @c netrc_path is set and exists, it is used.  A configured path that
 *      does not exist is logged as a warning and auto-discovery follows.
 *   2. Otherwise @c ~/.netrc (Unix convention).
 *   3. Otherwise @c ~/_netrc (Windows convention).
 *   4. Otherwise a warning is logged; CDDIS downloads then fail authentication.
 * The home directory is @c HOME, falling back to @c USERPROFILE.
 *
 * The resolved path is passed to libcurl via @c CURLOPT_NETRC_FILE so the
 * behaviour is the same on every platform.  libcurl's default file discovery
 * differs between Windows and Unix and finds nothing, without an error, when
 * the file uses the other platform's naming convention.
 */
class DataDownloader {
public:
    /**
     * @brief Construct a DataDownloader.
     * @param log_manager Logging facility.
     * @param netrc_path  Path to the .netrc credentials file.  If empty,
     *                    auto-discovery checks @c ~/.netrc then @c ~/_netrc.
     * @param keep_zip_after_unzip
     *                    When @c false (the default), @c downloadAndDecompress
     *                    deletes the @c .gz file once decompression succeeds.
     *                    No downstream consumer reads the compressed file: the
     *                    decompressed product is cached in the unzip directory,
     *                    and a later request after a manual delete downloads
     *                    it again.  Set to @c true to retain the .gz alongside
     *                    the decompressed copy (e.g. for an archival workflow
     *                    that needs the byte-exact original from CDDIS).
     */
    explicit DataDownloader(LogManager& log_manager,
                            const std::filesystem::path& netrc_path = {},
                            bool keep_zip_after_unzip = false);

    ~DataDownloader();

    /**
     * @brief Download a file from a URL to a local path.
     *
     * The parent directory of @p local_path is created if it does not exist.
     * If the file already exists it is not downloaded again and the call
     * returns true.  On a network error or an HTTP status of 400 or above,
     * the partial file is removed and the status is logged (with a .netrc
     * hint on 401/403).
     *
     * @param url        Full HTTPS URL.
     * @param local_path Destination file path.
     * @return True on success (or if file already existed), false on failure.
     */
    bool download(const std::string& url, const std::filesystem::path& local_path);

    /**
     * @brief Decompress a .gz file to an output path.
     *
     * If @p output_path already exists, decompression is skipped (returns true).
     *
     * @param gz_path     Path to the .gz file.
     * @param output_path Destination for the decompressed data.
     * @return True on success, false on failure.
     */
    bool decompressGz(const std::filesystem::path& gz_path,
                      const std::filesystem::path& output_path);

    /**
     * @brief Download and decompress a .gz file in one step.
     *
     * Combines download() and decompressGz().  The .gz file is stored in
     * @p zip_dir and the decompressed result is placed in @p unzip_dir under
     * the same base filename with the .gz (or .Z) extension removed.  If the
     * decompressed file already exists, nothing is downloaded.  After a
     * successful decompression the .gz file is deleted unless
     * @c keep_zip_after_unzip was set.
     *
     * @param url        Full HTTPS URL to the .gz file.
     * @param zip_dir    Directory for the compressed download.
     * @param unzip_dir  Directory for the decompressed output.
     * @return Path to the decompressed file, or empty path on failure.
     */
    std::filesystem::path downloadAndDecompress(const std::string& url,
                                                const std::filesystem::path& zip_dir,
                                                const std::filesystem::path& unzip_dir);

    /**
     * @brief Convert a Hatanaka-compressed .crx file to standard .rnx format.
     *
     * Runs the external crx2rnx tool. If the .rnx output already exists,
     * conversion is skipped.  A file without a recognized Hatanaka extension
     * (.crx, .CRX, or short-name .YYd / .YYD) is returned unchanged.
     *
     * @param crx_path     Path to the .crx input file.
     * @param crx2rnx_exe  Path to the crx2rnx executable.  If empty or missing,
     *                     well-known install directories and then PATH are
     *                     searched.
     * @return Path to the resulting .rnx file, or empty path on failure.
     */
    std::filesystem::path convertCrxToRnx(const std::filesystem::path& crx_path,
                                          const std::filesystem::path& crx2rnx_exe);

private:
    LogManager& log_manager_;
    std::filesystem::path netrc_path_;       ///< Resolved .netrc path (empty = none found).
    bool netrc_resolved_{ false };           ///< True once @c resolveNetrcPath() ran.
    bool keep_zip_after_unzip_{ false };

    /// Resolve and validate the .netrc location once per object. Logs the
    /// outcome at info (found) or warn (not found). Idempotent.
    void resolveNetrcPath();
};

} // namespace GnssProcessor
