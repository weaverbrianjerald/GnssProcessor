#pragma once

#include "logging/LogManager.hpp"
#include <cstddef>
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
 *
 * Content check.  A transfer that ends with HTTP 200 is not taken as the file
 * it names until its first bytes have been read (@c contentRefusal).  The
 * Earthdata login answers an unauthenticated request with its HTML login page
 * at status 200, and zlib's @c gzread copies a file that is not gzip data
 * through unchanged, so without the check the page would be saved and then
 * written out as the product.  A body that is an HTML page, or a @c .gz that
 * does not start with the gzip magic bytes, is refused: the file is removed,
 * the reason and the netrc hint are logged, and the call fails.  A file
 * already on disk is put to the same test and is removed and fetched again
 * when it fails.
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
     * If the file already exists and passes the content check it is not
     * downloaded again and the call returns true; one that fails the check is
     * removed and downloaded again.  On a network error, an HTTP status of 400
     * or above, or a response refused by @c contentRefusal (an HTML page, or
     * not gzip data for a @c .gz name), the partial file is removed and the
     * cause is logged (with a .netrc hint on 401/403 and on a refused page).
     *
     * @param url        Full HTTPS URL.
     * @param local_path Destination file path.
     * @return True on success (or if a valid file already existed), false on failure.
     */
    bool download(const std::string& url, const std::filesystem::path& local_path);

    /**
     * @brief Why a body that begins with @p head is not the file @p name.
     *
     * A body is refused when it is an HTML page (its first non-blank
     * character is @c '<' and either @p content_type is an HTML media type or
     * the bytes carry an HTML marker such as @c "<html" or @c "<title"), or
     * when @p name ends in @c .gz and the body does not start with the gzip
     * magic bytes @c 1f @c 8b.  None of the files fetched (gzip, SP3, CLK,
     * BIA, ERP, RINEX, ATX, netCDF) starts with @c '<', and a body that does
     * not is never refused on its Content-Type alone.  The rule is the one of
     * @c refusal_reason in @c scripts/bootstrap_internal_data.py.
     *
     * @param name         Destination file name (only its suffix is read).
     * @param head         The first bytes of the body (up to @c kSniffBytes).
     * @param content_type The response's Content-Type header, or empty.
     * @return Empty when the body may be the file, otherwise the reason it is
     *         not (for the log), naming the page title when there is one.
     */
    static std::string contentRefusal(const std::string& name,
                                      const std::string& head,
                                      const std::string& content_type = {});

    /**
     * @brief @c contentRefusal applied to the first bytes of a file on disk.
     *
     * @param path         File to read; its file name is the @c name tested.
     * @param content_type The response's Content-Type header, or empty.
     * @return Empty when the file may be what its name says (or cannot be
     *         read), otherwise the reason it is not.
     */
    static std::string fileContentRefusal(const std::filesystem::path& path,
                                          const std::string& content_type = {});

    /// Bytes of a body (or a file on disk) that the content check reads.
    static constexpr std::size_t kSniffBytes = 4096;

    /**
     * @brief Decompress a .gz file to an output path.
     *
     * If @p output_path already exists and is not an HTML page, decompression
     * is skipped (returns true); an HTML page there is removed and replaced.
     * A @p gz_path that fails @c fileContentRefusal (an HTML page, or a @c .gz
     * name without the gzip magic bytes) is refused (returns false): zlib
     * would otherwise copy it through unchanged.
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
     * decompressed file already exists and is not an HTML page, nothing is
     * downloaded; an HTML page there is removed and fetched again.  After a
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
