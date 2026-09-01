/**
 * @file RtStateSnapshot.hpp
 * @brief RT-engine filter-state snapshot (the consumer-side warm start — the
 *        RTB-7 complement).
 *
 * The network producer's RTB-7 snapshot deliberately excludes per-station
 * filter state; the consumer / user engine's own Kalman memory (position,
 * clock, ZWD, per-satellite iono, receiver biases, carrier AMBIGUITIES and the
 * full covariance) is exactly what re-converges for the first hours after
 * every restart.  This snapshot serializes that memory plus the engine's SSR
 * continuity baselines (solution-generation id, phase/code-bias applied maps,
 * the StateManager's discontinuity-counter memory), so a resumed engine keeps
 * its datum-break DETECTORS armed against everything the producer did during
 * the outage — the invariant-#7 re-coupling edge is re-validated by the
 * engine's own existing reactions rather than by new machinery.
 *
 * Format: line-tagged text like the RTB-7 network snapshot — strict
 * full-consumption parses, an END,<count> truncation guard, atomic
 * temp-file + rename writes.  Doubles are written round-trip exact ("{:.17g}").
 *
 *   # gnss_rt_state_snapshot v1
 *   M,week,tow,wall_utc,fingerprint_hex,core_size,state_size
 *   G,generation_id,epoch_count
 *   K,idx,kind,key            one per DYNAMIC state, ascending idx
 *   X,idx,value               one per state (core included)
 *   P,i,v0,...,vi             covariance lower-triangle row i
 *   D,key,counter             StateManager SSR disc-counter memory
 *   F,key,0|1                 A5 phase-bias applied map
 *   C,key,0|1                 A5 code-bias applied map
 *   END,n_lines
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
    uint64_t    fingerprint{ 0 };       ///< FNV-1a over the state-meaning config facts.
    std::size_t core_size{ 0 };         ///< Fixed core block length at serialization.
    std::size_t state_size{ 0 };        ///< Total state length at serialization.

    // --- G: engine continuity ---------------------------------------------
    uint32_t generation_id{ 0 };        ///< last_ssr_generation_id_ (0 = never seen).
    uint64_t epoch_count{ 0 };          ///< Processed-epoch counter (AR min-epoch gates).

    // --- K: dynamic layout (ascending index) ------------------------------
    struct DynState {
        std::size_t idx{ 0 };
        std::string kind;               ///< ION/IONR/AMB/RXCB/SATB/SATPB/RXPB/SATCLK.
        std::string key;                ///< The owning map's key, verbatim.
    };
    std::vector<DynState> dyn;

    // --- X/P: the filter memory -------------------------------------------
    Eigen::VectorXd x;                  ///< State vector (state_size).
    Eigen::MatrixXd P;                  ///< Covariance (state_size²), symmetric.

    // --- D/F/C: SSR continuity baselines ----------------------------------
    std::map<std::string, int>  ssr_disc;          ///< "sat|signal" -> counter.
    std::map<std::string, bool> phase_bias_applied; ///< A5 fresh/stale flip memory.
    std::map<std::string, bool> code_bias_applied;  ///< A5 code analog.
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
