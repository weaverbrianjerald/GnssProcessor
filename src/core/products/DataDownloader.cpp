/**
 * @file DataDownloader.cpp
 * @brief HTTP download (libcurl) and gzip decompression (zlib) utilities.
 *
 * Auth model: CDDIS (and other IGS hosts) authenticate via HTTP Basic + the
 * standard @c .netrc file format. This module makes the .netrc location
 * explicit (resolved once at construction, passed via @c CURLOPT_NETRC_FILE)
 * and surfaces HTTP-level errors loudly so the user can tell the difference
 * between "auth failed", "file not found on server", and "network failure"
 * — failure modes that previously all looked like a generic
 * "Download failed" warning.
 */

#include "products/DataDownloader.hpp"
#include <curl/curl.h>
#include <zlib.h>
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
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

DataDownloader::~DataDownloader() {
    curl_global_cleanup();
}

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

    // Auto-discover. Try the Unix convention first because that's what
    // most users on either OS actually have set up; fall through to
    // the Windows convention if not present.
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
// download
// ---------------------------------------------------------------------------

bool DataDownloader::download(const std::string& url,
                              const std::filesystem::path& local_path) {
    auto log = log_manager_.getLogger("downloader");
    resolveNetrcPath();

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
    // Note: we deliberately do NOT set CURLOPT_FAILONERROR. It aborts the
    // transfer before the body is written, which would deny us the ability
    // to log the error response (e.g. CDDIS's "Forbidden — authentication
    // required" HTML when .netrc credentials are wrong). We check the
    // response code manually below and log the body snippet on failure.

    // CDDIS authentication via .netrc — pass the resolved path explicitly.
    // CURL_NETRC_REQUIRED would error if no .netrc were available; with
    // OPTIONAL we still attempt the request (some endpoints are public)
    // but libcurl uses .netrc credentials when present.
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
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        log->error("Network error: {} for {}", curl_easy_strerror(res), url);
        std::filesystem::remove(local_path);
        return false;
    }

    if (http_code >= 400) {
        // Capture the body so the user can tell auth-failure from 404 from
        // a transient 5xx. CDDIS returns small HTML pages on 401/403 with
        // a clear "authentication required" message.
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
// Runs the external @c crx2rnx tool with stderr+stdout captured via popen
// (with @c 2>&1 in the shell command). On nonzero exit code or missing
// output, logs at error level with the full command, exit code, and the
// captured first few hundred bytes of crx2rnx output — replacing the
// previous std::system() call which silently dropped diagnostics on the
// floor.
// ---------------------------------------------------------------------------

namespace {

    /// Capture-output subprocess runner. Runs the shell command, returns
    /// {exit_code, captured_output}. The command is expected to redirect
    /// stderr -> stdout via "2>&1" in the caller. On Windows uses _popen,
    /// on Unix uses popen.
    struct RunResult {
        int exit_code{ -1 };
        std::string output;
    };

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
        // _pclose / pclose returns the wait status; on Windows the value is
        // the exit code directly. On POSIX, decode with WEXITSTATUS, but
        // since we don't pull in <sys/wait.h> in headers and this code
        // ships on Windows primarily, we accept that on POSIX a nonzero
        // value means failure even if the exact code is wrapped.
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

        // PATH lookup as a last resort. We probe with the same captured-
        // output runner so we can log whatever stderr says about why the
        // lookup failed (rare, but useful when PATH is being shadowed).
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
    // cmd.exe (which _popen invokes via "cmd /c <string>") strips one
    // leading and one trailing `"` from the command string before
    // executing — an idiosyncratic rule documented under "If /S is
    // not specified" in `cmd /?`.  When the command starts with a
    // quoted exe path AND has a quoted argument, that rule strips
    // the wrong pair and leaves a malformed line like
    //     C:/.../crx2rnx.exe" "C:\...\foo.crx 2>&1
    // which produces the classic "The filename, directory name, or
    // volume label syntax is incorrect" diagnostic.  Wrap the entire
    // command in an extra outer pair of quotes so the rule strips
    // those and the inner quoting survives intact.
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
