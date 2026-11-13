/**
 * @file RtStateSnapshot.hpp
 * @brief RT-engine filter-state snapshot: the consumer-side warm start, the
 *        complement of the RTB-7 network snapshot.
 *
 * The network producer's RTB-7 snapshot excludes per-station filter state by
 * design.  The consumer / user engine's own Kalman memory (position, clock, ZWD,
 * per-satellite iono, receiver biases, carrier ambiguities and the full
 * covariance) is what otherwise re-converges for the first hours after every
 * restart.  This snapshot serializes that memory plus the engine's SSR
 * continuity baselines (solution-generation id, phase/code-bias applied maps,
 * the StateManager's discontinuity-counter memory), so a resumed engine keeps
 * its datum-break detectors armed against everything the producer did during
 * the outage.  The invariant-#7 re-coupling edge is then re-validated by the
 * engine's existing reactions, with no additional mechanism.
 *
 * Format: line-tagged text like the RTB-7 network snapshot, with strict
 * full-consumption parses, an END,\<count\> truncation guard (the count of record
 * lines between the header and END) and atomic temp-file + rename writes.
 * Doubles are written round-trip exact ("{:.17g}").
 *
 *   # gnss_rt_state_snapshot v1
 *   M,week,tow,wall_utc,fingerprint_hex,core_size,state_size
 *   G,generation_id,epoch_count
 *   K,idx,kind,key            one per dynamic state, ascending idx
 *   X,idx,value               one per state (core included)
 *   P,i,v0,...,vi             covariance lower-triangle row i
 *   D,key,counter             StateManager SSR disc-counter memory
 *   F,key,0|1                 A5 phase-bias applied map
 *   C,key,0|1                 A5 code-bias applied map
 *   I,latch,0|1               StateInitializer seed latch (card CRS2; isb, phase_clock,
 *                             code_bias, clock_seed), written under
 *                             realtime.state_snapshot_latch_capture
 *   END,n_lines
 *
 * The @c I rows (card CRS2) sit under the v1 header like the RS-3 rows of the network
 * snapshot: a reader that finds none restores no latch (the behaviour before the card,
 * in which a restored consumer re-ran its first-epoch seeds on the states it restored),
 * and a binary that predates them fails the parse on the unknown tag, the documented
 * cold-start signal.  Whether a resume applies them is the read knob
 * realtime.state_snapshot_latch_restore (@ref GnssProcessor::rtSnapshotLatchesToApply "rtSnapshotLatchesToApply" for the scope
 * rule).  Not in the fingerprint: the rows change what the first epoch does, not what a
 * serialized state means.
 *
 * Not in v1: the consumer's last-applied phase-bias value map (the
 * declared-step carry's Δ baseline, @c PbDiscCarryLedger).  A restored engine
 * therefore has the A5 applied flags but no applied values, so the first disc
 * bump or freshness flip per key after a restore falls back to the existing
 * ambiguity reset (counted as @c pb_disc_reset_first_sight), which is the
 * conservative direction.  Every later event on that key is carried, because the
 * first post-restore epoch records the applied value.
 *
 * @see src/core/realtime/RealTimeEngineStateSnap.cpp (the engine seam that
 *      writes / stages / applies), docs/record/SPEC_synthetic_minimum_network.md §17.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include <Eigen/Dense>

namespace GnssProcessor {

/// In-memory image of one RT filter-state snapshot.
struct RtStateSnapshot {
    // --- M: meta -----------------------------------------------------------
    int         snap_week{ 0 };         ///< GPS week of the epoch the snapshot was taken at.
    double      snap_tow{ 0.0 };        ///< GPS time of week of that epoch (s).
    std::string wall_utc;              ///< Informational only.
    uint64_t    fingerprint{ 0 };       ///< FNV-1a over the config facts that fix what the state means.
    std::size_t core_size{ 0 };         ///< Fixed core block length at serialization.
    std::size_t state_size{ 0 };        ///< Total state length at serialization.

    // --- G: engine continuity ---------------------------------------------
    uint32_t generation_id{ 0 };        ///< last_ssr_generation_id_ (0 = never seen).
    uint64_t epoch_count{ 0 };          ///< Processed-epoch counter (AR min-epoch gates).

    // --- K: dynamic layout (ascending index) ------------------------------
    /// One dynamic state of the layout: its slot in the state vector and the key that owns it.
    struct DynState {
        std::size_t idx{ 0 };           ///< Index of the state in the state vector (at or beyond @c core_size).
        std::string kind;               ///< A family's snapshot tag (kStateFamilies, filter/StateFamily.hpp).
        std::string key;                ///< The owning map's key, verbatim.
    };
    std::vector<DynState> dyn;          ///< The dynamic states in ascending index order, one per state beyond the core block.

    // --- X/P: the filter memory -------------------------------------------
    Eigen::VectorXd x;                  ///< State vector (state_size).
    Eigen::MatrixXd P;                  ///< Covariance (state_size²), symmetric.

    // --- D/F/C: SSR continuity baselines ----------------------------------
    std::map<std::string, int>  ssr_disc;          ///< "sat|signal" -> counter.
    std::map<std::string, bool> phase_bias_applied; ///< Fresh/stale flip memory for phase biases (A5).
    std::map<std::string, bool> code_bias_applied;  ///< The code-bias counterpart (A5).

    // --- I: the initializer's seed latches (card CRS2) ----------------------
    /// Latch name (one of @ref kRtSnapshotLatchNames) -> set.  Empty on a file written
    /// without realtime.state_snapshot_latch_capture.
    std::map<std::string, bool> init_latches;
};

/// The latch names an @c I record may carry, in the writer's order: the four latches of
/// @c StateInitializer (the inter-system bias seed, the phase-clock seed, the receiver
/// code-bias seed, the cold receiver-clock seed).  The parser refuses any other name.
inline constexpr const char* kRtSnapshotLatchNames[4] = {"isb", "phase_clock", "code_bias",
                                                          "clock_seed"};

/**
 * @brief The latches a restore sets (card CRS2).
 *
 * Every latch @p latches holds true, in the order of @ref GnssProcessor::kRtSnapshotLatchNames "kRtSnapshotLatchNames", except
 * that @c code_bias is set under the full restore scope only: the core scope re-creates
 * the receiver code-bias states per epoch, and a state created after the restore needs
 * its first-epoch seed.  Pure, so the rule has a unit test of its own.
 * @param latches     The snapshot's @c I rows.
 * @param full_scope  realtime.state_snapshot_restore_scope == "full".
 * @return The names to set, possibly empty.
 */
std::vector<std::string> rtSnapshotLatchesToApply(const std::map<std::string, bool>& latches,
                                                  bool full_scope);

/// Serializer / strict parser / atomic file IO for @ref RtStateSnapshot.
class RtStateSnapshotIO {
public:
    /**
     * @brief Serialize @p snap to @p os (header through END line, '\n' terminated).
     * @param[in,out] os   Output stream; written to, not flushed or closed here.
     * @param         snap The snapshot to write; its @c wall_utc is written as given.
     */
    static void serialize(std::ostream& os, const RtStateSnapshot& snap);

    /**
     * @brief Strict parse (full-consumption fields, END count check).  On failure
     *        returns false and, when @p err is non-null, a one-line reason.
     * @param[in,out] is  Input stream positioned at the header line; consumed up to the END line.
     * @param[out]    out The parsed snapshot; assigned only on success, left unchanged on failure.
     * @param[out]    err Optional one-line failure reason; may be null, untouched on success.
     * @return True when the whole text parsed and the END count matched; false otherwise.
     */
    static bool parse(std::istream& is, RtStateSnapshot& out, std::string* err);

    /**
     * @brief Atomic write: temp file beside @p path + rename (binary mode, so the
     *        on-disk bytes match serialize() on every platform).
     * @param path        Destination file; replaced as a whole, never partially written.
     * @param snap        The snapshot to write; an empty @c wall_utc is stamped with the current UTC time.
     * @param[out] err    Optional one-line failure reason; may be null.
     * @return True when the file was written and moved into place; false on any I/O failure.
     */
    static bool write(const std::filesystem::path& path, const RtStateSnapshot& snap,
                      std::string* err);

    /**
     * @brief Read + parse @p path.
     * @param path     Snapshot file to open (binary mode).
     * @param[out] out The parsed snapshot; assigned only on success.
     * @param[out] err Optional one-line failure reason (cannot open, or the parse reason); may be null.
     * @return True when the file opened and parsed; false otherwise.
     */
    static bool read(const std::filesystem::path& path, RtStateSnapshot& out,
                     std::string* err);
};

} // namespace GnssProcessor
