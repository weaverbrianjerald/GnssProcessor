#pragma once
/**
 * @file CsvRotator.hpp
 * @brief Size-based rotation for always-appended CSV trace files (P1, RT service).
 *
 * A 24/7 network-producer service appends to a few CSV traces indefinitely
 * (@c network_trace.csv per epoch, @c network_solution.csv per satellite per
 * epoch, @c network_service_health.csv per heartbeat), which grow without bound
 * on a service host.  This helper implements numbered-suffix log rotation
 * behind @c network.trace_rotate_mb: when the live file exceeds the threshold
 * it is closed, the suffix chain shifts one place
 * (<tt>file.csv.k → file.csv.(k+1)</tt>, oldest deleted), the live file becomes
 * <tt>file.csv.1</tt>, and a fresh live file is reopened with the header row
 * re-written, so every rolled generation is independently parseable.
 *
 * Off (threshold <= 0, the default) is a strict no-op: nothing is stat'ed,
 * closed, or renamed, so an unrotated run's files are byte-identical.
 *
 * Operator note: the pyviz viewers auto-discover only the live file name
 * (e.g. @c network_trace.csv); no viewer reads the rotated
 * <tt>.1 … .keep</tt> suffixes.  To inspect history across a rotation
 * boundary, concatenate the chain oldest-first with the header lines of the
 * non-first generations stripped (or point the viewer at one generation at a
 * time).  See @c docs/rt_service_operations.md.
 *
 * Pure std::filesystem + std::ofstream (no logging dependencies) so it is
 * unit-testable against a temp directory without a network run.
 */

#include <filesystem>
#include <fstream>
#include <string>

namespace GnssProcessor {

/**
 * @brief Shift the rotated-suffix chain of @p file one place and retire the live file.
 *
 * Performs, in order (all non-throwing, best-effort per step):
 *  1. stage the live file: rename <tt>file → file.rotating</tt> (after clearing
 *     any stale scratch from a crashed roll).  This happens before any history
 *     change, so if the live file is locked the whole chain is left untouched
 *     and the call returns false with nothing lost.  (Shifting history first
 *     would delete one generation on every retry under a persistent live lock;
 *     m4.)
 *  2. delete <tt>file.(keep)</tt> (the oldest generation, if present),
 *  3. rename <tt>file.(k) → file.(k+1)</tt> for k = keep-1 … 1,
 *  4. rename <tt>file.rotating → file.1</tt>; on failure (a lock on <tt>.1</tt>)
 *     roll the staged file back to @p file so no rows are stranded.  Steps 2
 *     and 3 are not undone.
 *
 * The caller closes the live file first (an open handle blocks the staging
 * rename on Windows).  @p keep < 1 is treated as 1 (a single rolled
 * generation).  Missing intermediate generations are skipped silently; the
 * chain need not be dense.
 *
 * @param file Path of the live CSV file (e.g. <tt>logs/network_trace.csv</tt>).
 * @param keep Number of rolled generations to retain (<tt>.1 … .keep</tt>).
 * @return true iff the live file was fully retired to @c .1; false when the live
 *         file is missing/locked or the final slot could not be filled, in
 *         which case the live file is left (or restored) in place for a retry.
 */
bool rotateCsvSuffixes(const std::filesystem::path& file, int keep);

/**
 * @brief Size-gated in-place rotation of an open CSV output stream.
 *
 * When @p threshold_mb > 0, @p os is open, and the stream's write position has
 * reached <tt>threshold_mb · 2^20</tt> bytes: flush + close @p os, shift the
 * suffix chain via @ref rotateCsvSuffixes, reopen @p path fresh (truncate) and
 * re-write @p header so the new live file is independently parseable.  The
 * write position (@c tellp) is the size probe: the traces are opened truncated
 * and only appended, so it equals the file size without a filesystem stat per
 * call.
 *
 * Failure containment: if the live file cannot be retired (e.g. an external
 * tail-reader holds a Windows lock), the stream is reopened in append mode on
 * the original path so no rows are lost, and false is returned; a later call
 * retries once the lock clears.
 *
 * A strict no-op (returns false, touches nothing) when @p threshold_mb <= 0 or
 * @p os is not open, so a run with rotation off stays byte-identical.
 *
 * @param os           The live output stream (closed/reopened in place).
 * @param path         Path the stream writes to (needed for the renames/reopen).
 * @param header       Full CSV header line including the trailing newline,
 *                     re-written into every fresh live file.
 * @param threshold_mb Rotation threshold in MiB; <= 0 disables (no-op).
 * @param keep         Rolled generations to retain (see @ref rotateCsvSuffixes).
 * @return true iff a rotation was performed (fresh live file, header written).
 */
bool maybeRotateCsv(std::ofstream& os, const std::filesystem::path& path,
                    const std::string& header, double threshold_mb, int keep);

}  // namespace GnssProcessor
