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

    // 1. Stage the live file to a scratch name FIRST.  Retiring the live file is
    //    the only step whose failure matters (an external tail-reader lock), so
    //    doing it before ANY history mutation means a failed rename leaves the
    //    whole chain — and the live file — untouched to retry (m4: the previous
    //    order shifted/deleted history first, so a persistent lock eroded a
    //    generation on every retry while the live file never rotated).  It cannot
    //    go straight to .1 yet: .1 still holds the previous generation until the
    //    shift below frees it.
    const fs::path staged(file.string() + ".rotating");
    fs::remove(staged, ec);                          // clear any stale scratch from a crashed roll
    ec.clear();
    fs::rename(file, staged, ec);
    if (ec) return false;                            // live file locked — history intact, caller reopens & retries

    // 2. Retire the oldest generation so the shift below never renames onto an
    //    existing path (fs::rename onto an existing file is replace-semantics on
    //    MSVC but EEXIST elsewhere — deleting first keeps this portable).
    fs::remove(suffixPath(file, keep), ec);

    // 3. Shift the chain one place, newest-last so nothing is overwritten.
    for (int k = keep - 1; k >= 1; --k) {
        const fs::path from = suffixPath(file, k);
        ec.clear();
        if (!fs::exists(from, ec) || ec) continue;   // sparse chain: skip gaps
        fs::rename(from, suffixPath(file, k + 1), ec);
    }

    // 4. Move the staged live file into the now-free .1 slot.  If that fails — a
    //    reader holding .1 blocks both the shift above (which then left .1
    //    occupied) and this replace — roll the staged live file BACK to the live
    //    path so its rows are never stranded/lost: the caller reopens @p file and
    //    keeps appending, exactly as if rotation had not fired this cycle (matching
    //    the pre-fix live-lock behaviour, but now for a .1 lock).
    ec.clear();
    fs::rename(staged, suffixPath(file, 1), ec);
    if (ec) {
        std::error_code ec_back;
        fs::rename(staged, file, ec_back);   // best-effort restore of the live file
        return false;
    }
    return true;
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
