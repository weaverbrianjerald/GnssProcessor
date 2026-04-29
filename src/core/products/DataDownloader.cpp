/**
 * @file DataDownloader.cpp
 * @brief HTTP download (libcurl) and gzip decompression (zlib) utilities.
 */

#include "products/DataDownloader.hpp"
#include <curl/curl.h>
#include <zlib.h>
#include <fstream>
#include <vector>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// libcurl write callback
// ---------------------------------------------------------------------------
namespace {
    size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* out = static_cast<std::ofstream*>(userp);
        const size_t total = size * nmemb;
        out->write(static_cast<const char*>(contents), static_cast<std::streamsize>(total));
        return total;
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DataDownloader::DataDownloader(LogManager& log_manager,
                               const std::filesystem::path& netrc_path,
                               bool keep_zip_after_unzip)
    : log_manager_(log_manager),
      netrc_path_(netrc_path),
      keep_zip_after_unzip_(keep_zip_after_unzip) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

DataDownloader::~DataDownloader() {
    curl_global_cleanup();
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------

bool DataDownloader::download(const std::string& url,
                              const std::filesystem::path& local_path) {
    auto log = log_manager_.getLogger("downloader");

    // Skip if file already exists on disk.
    if (std::filesystem::exists(local_path)) {
        log->info("Already cached: {}", local_path.string());
        return true;
    }

    // Ensure parent directory exists.
    std::filesystem::create_directories(local_path.parent_path());

    CURL* curl = curl_easy_init();
    if (!curl) {
        log->error("Failed to initialize libcurl handle");
        return false;
    }

    std::ofstream ofs(local_path, std::ios::binary);
    if (!ofs.is_open()) {
        log->error("Cannot open output file: {}", local_path.string());
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ofs);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);         // HTTP >=400 is failure

    // CDDIS authentication via .netrc
    curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_REQUIRED);
    if (!netrc_path_.empty()) {
        curl_easy_setopt(curl, CURLOPT_NETRC_FILE, netrc_path_.string().c_str());
    }

    // Enable cookie engine (CDDIS redirects require cookies)
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    log->info("Downloading {} -> {}", url, local_path.string());
    CURLcode res = curl_easy_perform(curl);
    ofs.close();

    if (res != CURLE_OK) {
        log->warn("Download failed ({}): {}", curl_easy_strerror(res), url);
        // Remove partial file.
        std::filesystem::remove(local_path);
        curl_easy_cleanup(curl);
        return false;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code >= 400) {
        log->warn("HTTP {} for {}", http_code, url);
        std::filesystem::remove(local_path);
        return false;
    }

    log->info("Downloaded: {}", local_path.string());
    return true;
}

// ---------------------------------------------------------------------------
// decompressGz
// ---------------------------------------------------------------------------

bool DataDownloader::decompressGz(const std::filesystem::path& gz_path,
                                  const std::filesystem::path& output_path) {
    auto log = log_manager_.getLogger("downloader");

    if (std::filesystem::exists(output_path)) {
        log->info("Already decompressed: {}", output_path.string());
        return true;
    }

    std::filesystem::create_directories(output_path.parent_path());

    gzFile in = gzopen(gz_path.string().c_str(), "rb");
    if (!in) {
        log->error("Cannot open gz file: {}", gz_path.string());
        return false;
    }

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        log->error("Cannot open output file: {}", output_path.string());
        gzclose(in);
        return false;
    }

    constexpr int BUF_SIZE = 65536;
    std::vector<char> buf(BUF_SIZE);
    int bytes_read;
    while ((bytes_read = gzread(in, buf.data(), BUF_SIZE)) > 0) {
        out.write(buf.data(), bytes_read);
    }

    bool ok = (bytes_read == 0); // 0 means EOF, <0 means error
    gzclose(in);
    out.close();

    if (!ok) {
        log->error("Decompression error: {}", gz_path.string());
        std::filesystem::remove(output_path);
        return false;
    }

    log->info("Decompressed: {} -> {}", gz_path.string(), output_path.string());
    return true;
}

// ---------------------------------------------------------------------------
// downloadAndDecompress
// ---------------------------------------------------------------------------

std::filesystem::path DataDownloader::downloadAndDecompress(
    const std::string& url,
    const std::filesystem::path& zip_dir,
    const std::filesystem::path& unzip_dir) {

    // Derive filename from the URL (last path segment).
    std::string filename = url.substr(url.find_last_of('/') + 1);
    std::filesystem::path gz_path = zip_dir / filename;
    std::string unzipped_name = filename;

    // Strip ".gz" or ".Z" extension to get uncompressed name.
    if (unzipped_name.size() > 3 && unzipped_name.substr(unzipped_name.size() - 3) == ".gz") {
        unzipped_name = unzipped_name.substr(0, unzipped_name.size() - 3);
    } else if (unzipped_name.size() > 2 && unzipped_name.substr(unzipped_name.size() - 2) == ".Z") {
        unzipped_name = unzipped_name.substr(0, unzipped_name.size() - 2);
    }
    std::filesystem::path out_path = unzip_dir / unzipped_name;

    // If already decompressed, return immediately.
    if (std::filesystem::exists(out_path)) {
        log_manager_.getLogger("downloader")->info("Already available: {}", out_path.string());
        return out_path;
    }

    if (!download(url, gz_path)) return {};
    if (!decompressGz(gz_path, out_path)) return {};

    // Reclaim disk after a successful round-trip.  The decompressed
    // file is now the canonical cached artifact; the .gz is dead
    // weight unless the user explicitly opted in to retaining it.
    // Roughly halves the on-disk footprint of a typical product cache.
    if (!keep_zip_after_unzip_) {
        std::error_code ec;
        if (std::filesystem::remove(gz_path, ec)) {
            log_manager_.getLogger("downloader")->info(
                "Removed cached .gz after decompress: {}", gz_path.string());
        } else if (ec) {
            // Non-fatal: the unzip succeeded, the user just keeps the
            // .gz on disk this cycle.
            log_manager_.getLogger("downloader")->debug(
                "Could not remove {} ({}); leaving in place",
                gz_path.string(), ec.message());
        }
    }
    return out_path;
}

// ---------------------------------------------------------------------------
// convertCrxToRnx
// ---------------------------------------------------------------------------

std::filesystem::path DataDownloader::convertCrxToRnx(
    const std::filesystem::path& crx_path,
    const std::filesystem::path& crx2rnx_exe) {

    auto log = log_manager_.getLogger("downloader");

    // Derive the expected .rnx output path (same directory, .crx -> .rnx).
    std::filesystem::path rnx_path = crx_path;
    std::string stem = rnx_path.filename().string();
    auto pos = stem.rfind(".crx");
    if (pos == std::string::npos) {
        pos = stem.rfind(".CRX");
    }
    if (pos != std::string::npos) {
        stem.replace(pos, 4, ".rnx");
    } else {
        // Not a .crx file; nothing to convert.
        return crx_path;
    }
    rnx_path = crx_path.parent_path() / stem;

    // Skip if already converted.
    if (std::filesystem::exists(rnx_path)) {
        log->info("Already converted: {}", rnx_path.string());
        return rnx_path;
    }

    // Resolve crx2rnx executable: use explicit path, or search common locations.
    std::filesystem::path exe = crx2rnx_exe;
    if (exe.empty() || !std::filesystem::exists(exe)) {
        // Search well-known filesystem locations first.
        const std::vector<std::filesystem::path> search_paths = {
            "C:/Program Files/crx2rnx/crx2rnx.exe",
        };
        exe.clear();
        for (const auto& candidate : search_paths) {
            if (std::filesystem::exists(candidate)) {
                exe = candidate;
                break;
            }
        }

        // Fall back to PATH lookup via `where` (Windows) or `which` (Unix).
        if (exe.empty()) {
#ifdef _WIN32
            const char* which_cmd = "where crx2rnx.exe >nul 2>&1";
            const char* exe_name  = "crx2rnx.exe";
#else
            const char* which_cmd = "which crx2rnx >/dev/null 2>&1";
            const char* exe_name  = "crx2rnx";
#endif
            if (std::system(which_cmd) == 0) {
                exe = exe_name;   // found on PATH; std::system() will resolve it
            }
        }

        if (exe.empty()) {
            log->error("crx2rnx executable not found (configure data_download.crx2rnx_path)");
            return {};
        }
    }

    // crx2rnx writes the .rnx file next to the .crx input by default.
    // Run:  crx2rnx <input.crx>
    std::string cmd = "\"" + exe.string() + "\" \"" + crx_path.string() + "\"";
    log->info("Converting CRX -> RNX: {}", cmd);

    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        log->error("crx2rnx failed with exit code {}", ret);
        return {};
    }

    if (!std::filesystem::exists(rnx_path)) {
        log->error("crx2rnx did not produce expected output: {}", rnx_path.string());
        return {};
    }

    log->info("Converted: {} -> {}", crx_path.string(), rnx_path.string());
    return rnx_path;
}

} // namespace GnssProcessor
