#pragma once
/**
 * @file NetworkProductIO.hpp
 * @brief On-disk (de)serialization of the PPP-RTK NETWORK joint solution.
 *
 * Sink A of the network product-emit path (@c NetworkProductEmitter) pushes each
 * epoch's estimated satellite-side solution into an in-memory
 * @c SsrCorrectionStore for the in-process closed-loop consumer.  This module
 * adds the *persistent* half: a writer that appends each @c NetworkSolutionEpoch
 * to a plain-text product file as the producer runs, and a reader that parses it
 * back so consumer-side experiments (notably the consumer AR-frame work) can
 * replay the products WITHOUT re-running the expensive multi-station network.
 *
 * What is serialized is the *pre-emit* @c NetworkSolutionEpoch (estimable
 * satellite clock + code/phase bias) plus the per-epoch @c datum_changed flag —
 * the one piece of stream state the epoch struct itself does not carry (it lives
 * in the driver, set on a clock-pivot handover).  The discontinuity counters are
 * NOT serialized: they are stream state owned by @c NetworkProductEmitter and are
 * recomputed deterministically on replay.  Feeding the parsed records back
 * through the SAME @c NetworkProductEmitter::emitToStore (via @c replay, which
 * also re-applies @c markDatumChange) therefore reproduces the producer's live
 * @c SsrCorrectionStore contents byte-for-byte, discontinuity counters included.
 *
 * File format (line-based, '#' comments, comma-separated; one epoch is an @c E
 * line followed by its per-satellite @c C / @c B / @c P records):
 * @code
 *   # gnss_network_product v1
 *   E,<week>,<tow>,<iod_ssr>,<provider_id>,<solution_id>,<datum_changed 0|1>
 *   C,<sat>,<clock_c0_m>                              # satellite clock (has_clock)
 *   B,<sat>,<signal>,<code_bias_m>                    # satellite code bias
 *   P,<sat>,<signal>,<bias_m>,<signal_integer 0|1>,<wide_lane_integer>
 * @endcode
 * Floating-point fields are written with the shortest round-trippable decimal
 * (fmt's default formatting), so a parse recovers the exact @c double.  A
 * satellite with no @c C line round-trips to @c has_clock=false; a satellite with
 * no records at all is dropped (it would push nothing through @c emitToStore, so
 * the store result is identical).
 */

#include "products/NetworkProductEmitter.hpp"   // NetworkSolutionEpoch, NetworkProductEmitter

#include <fstream>
#include <string>
#include <vector>

namespace GnssProcessor {

class SsrCorrectionStore;

/// One serialized network-product record: the epoch's solution plus the
/// driver-side @c datum_changed flag (a clock-pivot handover this epoch), which
/// @c NetworkProductReader::replay turns back into a forced discontinuity bump.
struct NetworkProductRecord {
    NetworkSolutionEpoch epoch;                 ///< The estimable satellite-side solution.
    bool                 datum_changed{false};  ///< Clock-pivot / S-basis re-declaration this epoch.
};

/**
 * @class NetworkProductWriter
 * @brief Appends @c NetworkSolutionEpoch records to a product file.
 *
 * Opens the file (truncating) and writes the format header on construction, then
 * appends one block per @c write call.  The stream is flushed/closed when the
 * writer is destroyed.  Check @c good() after construction; a failed open leaves
 * the writer inert (every @c write is a no-op) so the producer can degrade
 * gracefully rather than abort.
 */
class NetworkProductWriter {
public:
    /// Open @p path for writing (truncate) and emit the format header.
    explicit NetworkProductWriter(const std::string& path);

    /// True iff the output stream is open and healthy.
    bool good() const { return static_cast<bool>(out_); }

    /// Path the writer was constructed with (for diagnostics).
    const std::string& path() const { return path_; }

    /// Append one epoch.  @p datum_changed records a clock-pivot handover this
    /// epoch so a replay can reproduce the forced discontinuity bump.  No-op if
    /// the stream is not @c good().
    void write(const NetworkSolutionEpoch& sol, bool datum_changed = false);

private:
    std::string   path_;
    std::ofstream out_;
};

/**
 * @class NetworkProductReader
 * @brief Parses a product file and replays it into an @c SsrCorrectionStore.
 */
class NetworkProductReader {
public:
    /**
     * @brief Parse every epoch from @p path.
     * @param path Product file written by @c NetworkProductWriter.
     * @param ok   Optional out-flag: set false on an open failure or any
     *             malformed line (parsing continues past a bad line), true
     *             otherwise.
     * @return Records in file order (epoch order).  Empty if the file cannot be
     *         opened.
     */
    static std::vector<NetworkProductRecord> read(const std::string& path,
                                                  bool* ok = nullptr);

    /**
     * @brief Replay one record into @p store via @p emitter, reproducing the live
     *        producer path: a @c markDatumChange (when @c rec.datum_changed) then
     *        @c emitToStore.  The consumer interleaves this with its own per-epoch
     *        processing exactly as @c runNetwork does live, so the store always
     *        reflects the just-replayed epoch.
     */
    static void replay(const NetworkProductRecord& rec,
                       SsrCorrectionStore& store,
                       NetworkProductEmitter& emitter);
};

} // namespace GnssProcessor
