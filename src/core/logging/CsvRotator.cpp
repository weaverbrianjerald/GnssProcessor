/**
 * @file CsvRotator.cpp
 * @brief Size-based numbered-suffix rotation for always-appended CSV traces (P1).
 */

#include "logging/CsvRotator.hpp"

#include <ios>
#include <system_error>

namespace GnssProcessor {

namespace {

/// <tt>file</tt> + "." + <tt>k</tt> — the k-th rolled generation's path.
std::filesystem::path suffixPath(const std::filesystem::path& file, int k) {
    return std::filesystem::path(file.string() + "." + std::to_string(k));
}

}  // namespace

bool rotateCsvSuffixes(const std::filesystem::path& file, int keep) {
    namespace fs = std::filesystem;
    if (keep < 1) keep = 1;
    std::error_code ec;

    // 1. Retire the oldest generation so the shift below never renames onto an
    //    existing path (fs::rename onto an existing file is replace-semantics on
    //    MSVC but EEXIST elsewhere — deleting first keeps this portable).
    fs::remove(suffixPath(file, keep), ec);

    // 2. Shift the chain one place, newest-last so nothing is overwritten.
    for (int k = keep - 1; k >= 1; --k) {
        const fs::path from = suffixPath(file, k);
        ec.clear();
        if (!fs::exists(from, ec) || ec) continue;   // sparse chain: skip gaps
        fs::rename(from, suffixPath(file, k + 1), ec);
    }

    // 3. Retire the live file to .1 — the only step whose failure matters (the
    //    caller reopens the live path either way).
    ec.clear();
    fs::rename(file, suffixPath(file, 1), ec);
    return !ec;
}

bool maybeRotateCsv(std::ofstream& os, const std::filesystem::path& path,
                    const std::string& header, double threshold_mb, int keep) {
    if (threshold_mb <= 0.0 || !os.is_open()) return false;   // OFF ⇒ strict no-op

    const std::streampos pos = os.tellp();
    if (pos < std::streampos(0)) return false;                // stream in a fail state
    const double threshold_bytes = threshold_mb * 1024.0 * 1024.0;
    if (static_cast<double>(pos) < threshold_bytes) return false;

    os.flush();
    os.close();
    const bool retired = rotateCsvSuffixes(path, keep);
    os.clear();   // close() keeps sticky state bits; clear before reopen
    if (retired) {
        os.open(path, std::ios::out | std::ios::trunc);
        if (os) os << header;                                 // fresh file stays parseable
        return true;
    }
    // Live file could not be retired (external lock?) — keep appending to it so
    // no rows are lost; a later call retries once the lock clears.
    os.open(path, std::ios::out | std::ios::app);
    return false;
}

}  // namespace GnssProcessor
