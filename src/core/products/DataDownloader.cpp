/**
 * @file DataDownloader.cpp
 * @brief HTTP download (libcurl) and gzip decompression (zlib) utilities.
 *
 * Authentication: CDDIS (and other IGS hosts) use HTTP Basic with credentials
 * from a standard @c .netrc file.  The .netrc location is resolved explicitly
 * (once, on the first download, then passed via @c CURLOPT_NETRC_FILE).  HTTP
 * errors are logged by class, so an authentication failure (401/403), a file
 * missing on the server (404) and a network failure produce distinct messages.
 */

#include "products/DataDownloader.hpp"
#include "common/CurlGlobalGuard.hpp"
#include <curl/curl.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <spdlog/spdlog.h>

#ifdef _WIN32
  #define POPEN  _popen
  #define PCLOSE _pclose
#else
  #define POPEN  popen
  #define PCLOSE pclose
#endif

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// libcurl callbacks: file write + in-memory error-body capture
// ---------------------------------------------------------------------------
namespace {

    size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* out = static_cast<std::ofstream*>(userp);
        const size_t total = size * nmemb;
        out->write(static_cast<const char*>(contents), static_cast<std::streamsize>(total));
        return total;
    }

    /// Read at most @p max_bytes from a file as a string snippet for log diagnostics.
    /// Strips trailing newlines and replaces non-printable bytes with '.'.
    std::string readFileSnippet(const std::filesystem::path& p, std::size_t max_bytes = 200) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return {};
        std::vector<char> buf(max_bytes);
        f.read(buf.data(), static_cast<std::streamsize>(max_bytes));
        const auto n = static_cast<std::size_t>(f.gcount());
        std::string s;
        s.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c == '\n' || c == '\r' || c == '\t') s.push_back(' ');
            else if (c < 0x20 || c >= 0x7f) s.push_back('.');
            else s.push_back(static_cast<char>(c));
        }
        // Collapse runs of spaces and trim.
        std::string out;
        out.reserve(s.size());
        bool prev_space = false;
        for (char c : s) {
            if (c == ' ') {
                if (!prev_space) out.push_back(' ');
                prev_space = true;
            } else {
                out.push_back(c);
                prev_space = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    /// @p s in lower case (ASCII only; the length is unchanged).
    std::string asciiLower(std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return s;
    }

    /// True when @p s ends with @p suffix, ignoring ASCII case.
    bool endsWithNoCase(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size()
            && asciiLower(s.substr(s.size() - suffix.size())) == suffix;
    }

    /// The text of the first <title> element in @p head, blanks collapsed and
    /// cut at 80 characters, or empty when there is none.
    std::string htmlTitle(const std::string& head) {
        const std::string lower = asciiLower(head);
        const auto open = lower.find("<title");
        if (open == std::string::npos) return {};
        const auto start = lower.find('>', open);
        if (start == std::string::npos) return {};
        const auto end = lower.find("</title", start);
        if (end == std::string::npos) return {};
        std::string title;
        bool prev_blank = true;
        for (std::size_t i = start + 1; i < end && title.size() < 80; ++i) {
            const unsigned char c = static_cast<unsigned char>(head[i]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                if (!prev_blank) title.push_back(' ');
                prev_blank = true;
            } else {
                title.push_back(c < 0x20 || c == 0x7f ? '.' : static_cast<char>(c));
                prev_blank = false;
            }
        }
        while (!title.empty() && title.back() == ' ') title.pop_back();
        return title;
    }

    /// The first @p n bytes of @p s as lower-case hex pairs separated by blanks.
    std::string hexBytes(const std::string& s, std::size_t n) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        for (std::size_t i = 0; i < (std::min)(n, s.size()); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (!out.empty()) out.push_back(' ');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0f]);
        }
        return out;
    }

    /// Resolve the user's home directory using the OS-appropriate env var.
    /// Returns empty path if neither HOME nor USERPROFILE is set.
    std::filesystem::path getHomeDir() {
        if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h);
        if (const char* u = std::getenv("USERPROFILE"); u && *u) return std::filesystem::path(u);
        return {};
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
    // Process-wide call_once guard shared with NtripClient; a direct
    // curl_global_init here would run again on every construction (RTB-2(a)).
    ensureCurlGlobalInit();
}

// The destructor does not call curl_global_cleanup.  That call is not
// thread-safe and is valid only after all curl use in the process has ended,
// which this object cannot know; NtripClient receive threads may still be
// inside curl_easy_perform.  The libcurl documentation permits skipping the
// cleanup at process exit; see CurlGlobalGuard.hpp (RTB-2(a)).
DataDownloader::~DataDownloader() = default;

// ---------------------------------------------------------------------------
// .netrc resolution (idempotent; runs on first download attempt)
// ---------------------------------------------------------------------------

void DataDownloader::resolveNetrcPath() {
    if (netrc_resolved_) return;
    netrc_resolved_ = true;

    auto log = log_manager_.getLogger("downloader");

    // Honor an explicit config-supplied path first.
    if (!netrc_path_.empty()) {
        if (std::filesystem::exists(netrc_path_)) {
            log->info("auth: using configured .netrc at {}", netrc_path_.string());
            return;
        }
        log->warn("auth: configured netrc_path does not exist: {}", netrc_path_.string());
        netrc_path_.clear();
    }

    // Auto-discovery.  The Unix convention is tried first because it is the
    // common setup on either OS; the Windows convention is the fallback.
    const auto home = getHomeDir();
    if (home.empty()) {
        log->warn("auth: HOME / USERPROFILE not set; cannot locate .netrc — "
                  "CDDIS downloads will fail authentication");
        return;
    }
    const auto dot   = home / ".netrc";
    const auto under = home / "_netrc";
    if (std::filesystem::exists(dot)) {
        netrc_path_ = dot;
        log->info("auth: using .netrc at {}", netrc_path_.string());
        return;
    }
    if (std::filesystem::exists(under)) {
        netrc_path_ = under;
        log->info("auth: using _netrc at {}", netrc_path_.string());
        return;
    }
    log->warn("auth: no .netrc found (looked at {} and {}); "
              "CDDIS / EarthScope authenticated downloads will fail",
              dot.string(), under.string());
}

// ---------------------------------------------------------------------------
// Content check
// ---------------------------------------------------------------------------

std::string DataDownloader::contentRefusal(const std::string& name,
                                           const std::string& head,
                                           const std::string& content_type) {
    // HTML: the first non-blank character (after an optional UTF-8 BOM) is '<',
    // and the media type or a marker in the bytes says HTML.
    std::size_t first = (head.compare(0, 3, "\xEF\xBB\xBF") == 0) ? 3 : 0;
    first = head.find_first_not_of(" \t\r\n\v\f", first);
    if (first != std::string::npos && head[first] == '<') {
        std::string media = asciiLower(content_type.substr(0, content_type.find(';')));
        const auto b = media.find_first_not_of(" \t");
        const auto e = media.find_last_not_of(" \t");
        media = (b == std::string::npos) ? std::string{} : media.substr(b, e - b + 1);
        bool html = (media == "text/html" || media == "application/xhtml+xml");
        if (!html) {
            const std::string text = asciiLower(head.substr(first));
            for (const char* marker : {"<!doctype html", "<html", "<head", "<body", "<title"}) {
                if (text.find(marker) != std::string::npos) { html = true; break; }
            }
        }
        if (html) {
            const std::string title = htmlTitle(head);
            return title.empty() ? std::string("the response is an HTML page")
                                 : "the response is an HTML page titled \"" + title + "\"";
        }
    }
    // A .gz name must hold gzip data.
    if (endsWithNoCase(name, ".gz") && head.compare(0, 2, "\x1f\x8b") != 0) {
        return "the file is a .gz but the response is not gzip data ("
             + (head.empty() ? std::string("the body is empty")
                             : "it begins with bytes " + hexBytes(head, 4))
             + ")";
    }
    return {};
}

std::string DataDownloader::fileContentRefusal(const std::filesystem::path& path,
                                               const std::string& content_type) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string head(kSniffBytes, '\0');
    f.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(f.gcount()));
    return contentRefusal(path.filename().string(), head, content_type);
}

// ---------------------------------------------------------------------------
// download
// ---------------------------------------------------------------------------

bool DataDownloader::download(const std::string& url,
                              const std::filesystem::path& local_path) {
    auto log = log_manager_.getLogger("downloader");
    resolveNetrcPath();

    // Skip if a valid file already exists on disk.  An empty file, or one that
    // fails the content check (a login page saved by an earlier build), is
    // removed and fetched again.
    if (std::filesystem::exists(local_path)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(local_path, ec);
        const std::string reason = (!ec && size == 0) ? std::string("the file is empty")
                                                      : fileContentRefusal(local_path);
        if (reason.empty()) {
            log->info("Already cached: {}", local_path.string());
            return true;
        }
        log->warn("Discarding cached {} ({}); fetching it again", local_path.string(), reason);
        std::filesystem::remove(local_path);
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
    // CURLOPT_FAILONERROR is left unset on purpose: it aborts the transfer
    // before the body is written, and the error body (e.g. the CDDIS HTML page
    // that reports an authentication failure) is logged below.  The response
    // code is checked after the transfer instead.

    // CDDIS authentication via .netrc, with the resolved path passed explicitly.
    // CURL_NETRC_REQUIRED would fail when no .netrc is available; with OPTIONAL
    // the request is still attempted (some endpoints are public), and libcurl
    // uses the .netrc credentials when present.
    curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
    if (!netrc_path_.empty()) {
        curl_easy_setopt(curl, CURLOPT_NETRC_FILE, netrc_path_.string().c_str());
    }

    // Enable cookie engine (CDDIS redirects require cookies)
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    log->info("Downloading {} -> {}", url, local_path.string());
    CURLcode res = curl_easy_perform(curl);
    ofs.close();

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    char* effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    const std::string final_url = effective_url ? effective_url : url;
    char* content_type_c = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_c);
    const std::string content_type = content_type_c ? content_type_c : "";
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log->error("Network error: {} for {}", curl_easy_strerror(res), url);
        std::filesystem::remove(local_path);
        return false;
    }

    if (http_code >= 400) {
        // Log the start of the response body to separate an authentication
        // failure from a 404 or a transient 5xx.  CDDIS returns a small HTML
        // page on 401/403 that states that authentication is required.
        const std::string snippet = readFileSnippet(local_path);
        if (http_code == 401 || http_code == 403) {
            log->error("HTTP {} (auth failure) for {}", http_code, url);
            log->error("  Hint: check that your .netrc has a valid CDDIS entry. "
                       "See https://cddis.nasa.gov/Data_and_Derived_Products/CreateNetrcFile.html");
            log->error("  Resolved .netrc: {}", netrc_path_.empty() ? "<none>" : netrc_path_.string());
        } else if (http_code == 404) {
            log->warn("HTTP 404 (not found) for {}", url);
        } else {
            log->error("HTTP {} for {}", http_code, url);
        }
        if (final_url != url) log->error("  Final URL after redirects: {}", final_url);
        if (!snippet.empty()) log->error("  Server response (first ~200 chars): {}", snippet);
        std::filesystem::remove(local_path);
        return false;
    }

    // A 200 is not yet the file: the Earthdata login answers an unauthenticated
    // request with its HTML page at 200, and a .gz must hold gzip data.
    if (const std::string reason = fileContentRefusal(local_path, content_type);
        !reason.empty()) {
        log->error("Refused {}: {}", url, reason);
        if (final_url != url) log->error("  Final URL after redirects: {}", final_url);
        log->error("  Hint: check the urs.earthdata.nasa.gov entry of the .netrc. "
                   "See https://cddis.nasa.gov/Data_and_Derived_Products/CreateNetrcFile.html");
        log->error("  Resolved .netrc: {}", netrc_path_.empty() ? "<none>" : netrc_path_.string());
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
        const std::string reason = fileContentRefusal(output_path);
        if (reason.empty()) {
            log->info("Already decompressed: {}", output_path.string());
            return true;
        }
        log->warn("Discarding {} ({}); decompressing again", output_path.string(), reason);
        std::filesystem::remove(output_path);
    }

    // gzread copies a file that is not gzip data through unchanged, so an
    // HTML page, or a .gz input without the gzip magic, is refused here.
    if (const std::string reason = fileContentRefusal(gz_path); !reason.empty()) {
        log->error("Not decompressing {}: {}", gz_path.string(), reason);
        return false;
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

    // If already decompressed, return immediately; a saved HTML page is not a
    // product and is removed so it is fetched again.
    if (std::filesystem::exists(out_path)) {
        if (const std::string reason = fileContentRefusal(out_path); !reason.empty()) {
            log_manager_.getLogger("downloader")->warn(
                "Discarding {} ({}); fetching it again", out_path.string(), reason);
            std::filesystem::remove(out_path);
        }
    }
    if (std::filesystem::exists(out_path)) {
        log_manager_.getLogger("downloader")->info("Already available: {}", out_path.string());
        return out_path;
    }

    if (!download(url, gz_path)) return {};
    if (!decompressGz(gz_path, out_path)) return {};

    // After a successful download and decompression the decompressed file is
    // the cached artifact, so the .gz is deleted to save disk unless
    // keep_zip_after_unzip is set.  A failed delete is logged at debug level.
    if (!keep_zip_after_unzip_) {
        std::error_code ec;
        if (std::filesystem::remove(gz_path, ec)) {
            log_manager_.getLogger("downloader")->info(
                "Removed cached .gz after decompress: {}", gz_path.string());
        } else if (ec) {
            log_manager_.getLogger("downloader")->debug(
                "Could not remove {} ({}); leaving in place",
                gz_path.string(), ec.message());
        }
    }
    return out_path;
}

// ---------------------------------------------------------------------------
// convertCrxToRnx
//
// Runs the external @c crx2rnx tool with stderr and stdout captured via popen
// (@c 2>&1 in the shell command).  On a nonzero exit code or a missing output
// file, logs at error level the full command, the exit code, and the first
// lines of the captured crx2rnx output.
// ---------------------------------------------------------------------------

namespace {

    /// Result of runCaptured(): exit code and captured output.
    struct RunResult {
        int exit_code{ -1 };
        std::string output;
    };

    /// Capture-output subprocess runner.  Runs the shell command and returns
    /// {exit_code, captured_output}; the caller redirects stderr to stdout
    /// with "2>&1" in the command.  Uses _popen on Windows and popen on Unix.
    /// Captured output is truncated after about 16 KiB.
    RunResult runCaptured(const std::string& shell_cmd) {
        RunResult r;
        FILE* pipe = POPEN(shell_cmd.c_str(), "r");
        if (!pipe) return r;
        std::array<char, 4096> buf{};
        while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
            r.output.append(buf.data());
            if (r.output.size() > 16384) {  // cap captured output to avoid runaway
                r.output.append("\n[output truncated]");
                break;
            }
        }
        const int rc = PCLOSE(pipe);
        // _pclose / pclose returns the wait status.  On Windows the value is
        // the exit code directly.  On POSIX it would need WEXITSTATUS from
        // <sys/wait.h>, which is not included; the raw status is stored, so a
        // nonzero value still means failure but is not the tool's exit code.
        r.exit_code = rc;
        return r;
    }

    /// Log a multi-line captured-output block with a consistent prefix so
    /// it stands out in the .log file.
    void logCapturedOutput(const std::shared_ptr<spdlog::logger>& log,
                           const std::string& output) {
        if (output.empty()) {
            log->error("  (crx2rnx produced no output)");
            return;
        }
        // Emit up to first ~10 lines / 800 chars to keep logs readable.
        std::string head = output.substr(0, std::min<std::size_t>(output.size(), 800));
        std::stringstream ss(head);
        std::string line;
        int lines_emitted = 0;
        while (std::getline(ss, line) && lines_emitted < 10) {
            // Trim trailing CR (Windows pipes).
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty()) continue;
            log->error("  crx2rnx: {}", line);
            ++lines_emitted;
        }
    }

} // anonymous namespace

std::filesystem::path DataDownloader::convertCrxToRnx(
    const std::filesystem::path& crx_path,
    const std::filesystem::path& crx2rnx_exe) {

    auto log = log_manager_.getLogger("downloader");

    if (!std::filesystem::exists(crx_path)) {
        log->error("convertCrxToRnx: input does not exist: {}", crx_path.string());
        return {};
    }

    // Derive the expected .rnx output path.
    // Long-name: *.crx → *.rnx (extension swap).
    // Short-name: *.YYd → *.YYo / *.YYD → *.YYO (last-char swap, preserves case).
    std::filesystem::path rnx_path = crx_path;
    {
        std::string name = rnx_path.filename().string();
        auto pos_lower = name.rfind(".crx");
        auto pos_upper = name.rfind(".CRX");
        if (pos_lower != std::string::npos && pos_lower == name.size() - 4) {
            name.replace(pos_lower, 4, ".rnx");
            rnx_path = crx_path.parent_path() / name;
        } else if (pos_upper != std::string::npos && pos_upper == name.size() - 4) {
            name.replace(pos_upper, 4, ".RNX");
            rnx_path = crx_path.parent_path() / name;
        } else if (name.size() >= 4 && name[name.size() - 4] == '.'
                   && std::isdigit(static_cast<unsigned char>(name[name.size() - 3]))
                   && std::isdigit(static_cast<unsigned char>(name[name.size() - 2]))
                   && (name.back() == 'd' || name.back() == 'D')) {
            const char swap = (name.back() == 'd') ? 'o' : 'O';
            name[name.size() - 1] = swap;
            rnx_path = crx_path.parent_path() / name;
        } else {
            // Not a recognized Hatanaka extension; nothing to convert.
            return crx_path;
        }
    }

    // Skip if already converted.
    if (std::filesystem::exists(rnx_path)) {
        log->info("Already converted: {}", rnx_path.string());
        return rnx_path;
    }

    // Resolve crx2rnx executable: explicit path → well-known fallbacks → PATH.
    std::filesystem::path exe = crx2rnx_exe;
    if (exe.empty() || !std::filesystem::exists(exe)) {
        const std::vector<std::filesystem::path> search_paths = {
            "C:/GnssProcessorSupport/exe/crx2rnx.exe",
            "C:/Program Files/crx2rnx/crx2rnx.exe",
            "/usr/local/bin/crx2rnx",
            "/usr/bin/crx2rnx",
        };
        exe.clear();
        for (const auto& candidate : search_paths) {
            if (std::filesystem::exists(candidate)) {
                exe = candidate;
                break;
            }
        }

        // PATH lookup as a last resort, probed with the captured-output runner.
        // Only the probe's exit code is used; its output is not logged.
        if (exe.empty()) {
#ifdef _WIN32
            const std::string probe = "where crx2rnx.exe 2>&1";
            const char* exe_name    = "crx2rnx.exe";
#else
            const std::string probe = "which crx2rnx 2>&1";
            const char* exe_name    = "crx2rnx";
#endif
            const RunResult probe_res = runCaptured(probe);
            if (probe_res.exit_code == 0) {
                exe = exe_name;  // shell will resolve via PATH
            }
        }

        if (exe.empty()) {
            log->error("crx2rnx executable not found");
            log->error("  Configure data_download.crx2rnx_path in your JSON, install RNXCMP, or place crx2rnx on PATH");
            log->error("  Searched: configured path; well-known install dirs; PATH via {}",
#ifdef _WIN32
                       "where crx2rnx.exe"
#else
                       "which crx2rnx"
#endif
            );
            return {};
        }
    }

    // Build absolute paths so cwd doesn't matter. crx2rnx writes the .rnx
    // next to the .crx input, regardless of where it was invoked from.
    const std::filesystem::path abs_crx = std::filesystem::absolute(crx_path);
    std::ostringstream cmd;
#ifdef _WIN32
    // cmd.exe (which _popen invokes via "cmd /c <string>") strips one leading
    // and one trailing `"` from the command string before executing it (the
    // rule under "If /S is not specified" in `cmd /?`).  When the command
    // starts with a quoted exe path and also has a quoted argument, that rule
    // strips the wrong pair and leaves a malformed line such as
    //     C:/.../crx2rnx.exe" "C:\...\foo.crx 2>&1
    // which fails with "The filename, directory name, or volume label syntax
    // is incorrect".  The whole command is therefore wrapped in an extra outer
    // pair of quotes; the rule strips those and the inner quoting survives.
    cmd << "\"\"" << exe.string() << "\" \"" << abs_crx.string() << "\" 2>&1\"";
#else
    cmd << '"' << exe.string() << "\" \"" << abs_crx.string() << "\" 2>&1";
#endif

    log->info("Converting CRX -> RNX: {} {}", exe.string(), abs_crx.filename().string());

    const RunResult res = runCaptured(cmd.str());
    if (res.exit_code != 0) {
        log->error("crx2rnx failed with exit code {}", res.exit_code);
        log->error("  Command: {}", cmd.str());
        logCapturedOutput(log, res.output);
        return {};
    }

    if (!std::filesystem::exists(rnx_path)) {
        log->error("crx2rnx exited 0 but did not produce expected output: {}", rnx_path.string());
        log->error("  Command: {}", cmd.str());
        logCapturedOutput(log, res.output);
        return {};
    }

    log->info("Converted: {} -> {}", crx_path.filename().string(), rnx_path.filename().string());
    return rnx_path;
}

} // namespace GnssProcessor
