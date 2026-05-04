#pragma once

#include "logging/LogManager.hpp"
#include <filesystem>
#include <string>

namespace GnssProcessor {

/**
 * @brief Low-level HTTP download and .gz decompression utilities.
 *
 * Uses libcurl for HTTPS transfers and zlib for gzip decompression.
 * CDDIS authentication is handled via the standard .netrc mechanism
 * (libcurl reads ~/.netrc automatically when instructed).
 *
 * On construction the .netrc path is resolved explicitly:
 *   1. If @c netrc_path is set and exists, use it.
 *   2. Else look for @c ~/.netrc (Unix convention).
 *   3. Else look for @c ~/_netrc (Windows convention).
 *   4. Else log a warning (CDDIS downloads will fail).
 *
 * The resolved path is passed to libcurl via @c CURLOPT_NETRC_FILE so the
 * behaviour is deterministic across platforms — libcurl's default file
 * discovery differs between Windows and Unix and silently picks up nothing
 * if the user uses the "wrong" convention for their OS.
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
     *                    The compressed file isn't needed for any
     *                    downstream consumer — the decompressed product is
     *                    cached in the @c unzip/ subdir, and a re-fetch
     *                    after a manual delete just re-downloads.
     *                    Set to @c true to retain the .gz alongside the
     *                    decompressed copy (e.g. for a network-isolated
     *                    archival workflow that needs the byte-exact
     *                    original from CDDIS).
     */
    explicit DataDownloader(LogManager& log_manager,
                            const std::filesystem::path& netrc_path = {},
                            bool keep_zip_after_unzip = false);

    ~DataDownloader();

    /**
     * @brief Download a file from a URL to a local path.
     *
     * The parent directory of @p local_path is created if it does not exist.
     * If the file already exists it is NOT re-downloaded (returns true
     * immediately).
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
     * Combines download() and decompressGz(). The .gz file is stored in
     * @p zip_dir and the decompressed result is placed in @p unzip_dir
     * under the same base filename (without the .gz extension).
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
     * conversion is skipped.
     *
     * @param crx_path     Path to the .crx input file.
     * @param crx2rnx_exe  Path to the crx2rnx executable.
     * @return Path to the resulting .rnx file, or empty path on failure.
     */
    std::filesystem::path convertCrxToRnx(const std::filesystem::path& crx_path,
                                          const std::filesystem::path& crx2rnx_exe);

private:
    LogManager& log_manager_;
    std::filesystem::path netrc_path_;       ///< Resolved .netrc path (empty = none found).
    bool netrc_resolved_{ false };           ///< True once @c resolveNetrcPath() ran.
    bool keep_zip_after_unzip_{ false };

    /// Resolve and validate the .netrc location once per process. Logs the
    /// outcome at info (found) or warn (not found). Idempotent.
    void resolveNetrcPath();
};

} // namespace GnssProcessor
