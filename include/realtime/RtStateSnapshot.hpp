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
 * full-consumption parses, an END,<count> truncation guard (the count of record
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
 *   END,n_lines
 *
 * Not in v1 (card CC (A)): the consumer's last-applied phase-bias value map (the
 * declared-step carry's Δ baseline, @c PbDiscCarryLedger).  A restored engine
 * therefore has the A5 applied flags but no applied values, so the first disc
 * bump or freshness flip per key after a restore falls back to the existing
 * ambiguity reset (counted as @c pb_disc_reset_first_sight), which is the
 * conservative direction.  Every later event on that key is carried, because the
 * first post-restore epoch records the applied value.
 *
 * @see src/core/realtime/RealTimeEngineStateSnap.cpp (the engine seam that
 *      writes / stages / applies), docs/SPEC_synthetic_minimum_network.md §17.
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
    int         snap_week{ 0 };
    double      snap_tow{ 0.0 };
    std::string wall_utc;               ///< Informational only.
    uint64_t    fingerprint{ 0 };       ///< FNV-1a over the config facts that fix what the state means.
    std::size_t core_size{ 0 };         ///< Fixed core block length at serialization.
    std::size_t state_size{ 0 };        ///< Total state length at serialization.

    // --- G: engine continuity ---------------------------------------------
    uint32_t generation_id{ 0 };        ///< last_ssr_generation_id_ (0 = never seen).
    uint64_t epoch_count{ 0 };          ///< Processed-epoch counter (AR min-epoch gates).

    // --- K: dynamic layout (ascending index) ------------------------------
    struct DynState {
        std::size_t idx{ 0 };
        std::string kind;               ///< A family's snapshot tag (kStateFamilies, filter/StateFamily.hpp).
        std::string key;                ///< The owning map's key, verbatim.
    };
    std::vector<DynState> dyn;

    // --- X/P: the filter memory -------------------------------------------
    Eigen::VectorXd x;                  ///< State vector (state_size).
    Eigen::MatrixXd P;                  ///< Covariance (state_size²), symmetric.

    // --- D/F/C: SSR continuity baselines ----------------------------------
    std::map<std::string, int>  ssr_disc;          ///< "sat|signal" -> counter.
    std::map<std::string, bool> phase_bias_applied; ///< Fresh/stale flip memory for phase biases (A5).
    std::map<std::string, bool> code_bias_applied;  ///< The code-bias counterpart (A5).
};

/// Serializer / strict parser / atomic file IO for @ref RtStateSnapshot.
class RtStateSnapshotIO {
public:
    /// Serialize @p snap to @p os (header through END line, '\n' terminated).
    static void serialize(std::ostream& os, const RtStateSnapshot& snap);

    /// Strict parse (full-consumption fields, END count check).  On failure
    /// returns false and, when @p err is non-null, a one-line reason.
    static bool parse(std::istream& is, RtStateSnapshot& out, std::string* err);

    /// Atomic write: temp file beside @p path + rename (binary mode, so the
    /// on-disk bytes match serialize() on every platform).
    static bool write(const std::filesystem::path& path, const RtStateSnapshot& snap,
                      std::string* err);

    /// Read + parse @p path.
    static bool read(const std::filesystem::path& path, RtStateSnapshot& out,
                     std::string* err);
};

} // namespace GnssProcessor
