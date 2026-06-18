#pragma once
/**
 * @file NetworkProductEmitter.hpp
 * @brief Producer-side serializer for the PPP-RTK NETWORK joint solution.
 *
 * Sink A of the network product-emit path (W8): maps one epoch of the network's
 * estimated satellite-side solution (estimable satellite clock + code/phase
 * bias) into the existing SSR record structs and pushes them into a
 * @c SsrCorrectionStore via its push API — in the exact IGS-SSR convention the
 * USER consumer reads.  This makes the existing, battle-tested user pipeline the
 * closed-loop integer-recovery validation harness: "does our own user engine
 * recover integers from our own products on a held-out station?"
 *
 * The emitter does NOT estimate; it serializes a @c NetworkSolutionEpoch the
 * network estimator (W4) produces.  It DOES own the discontinuity bookkeeping —
 * stream state the estimator cannot know: it tracks the last emitted phase bias
 * per (sat, signal) and increments the @c discontinuity_counter on a bias jump
 * (or, via @c markDatumChange, on a global pivot / S-basis re-declaration, which
 * W6b wires in).  The consumer's @c noteSsrDiscontinuity -> @c resetAmbiguity
 * hook turns that increment into a clean ambiguity reset.
 *
 * Sink B (RTCM-4076 wire encoding) is deferred to Phase 2.
 *
 * NOTE: push into an OWNED (non-singleton) SsrCorrectionStore for the closed
 * loop — a producer must never share the store it (or its consumer) reads from
 * the global singleton, and tests must not pollute global state.
 */

#include "rtcm/RtcmTypes.hpp"   // GnssTime, SSR record structs

#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

class SsrCorrectionStore;

/// One satellite's estimable phase bias for a single signal (producer side).
struct NetworkPhaseBias {
    double bias_m{0.0};            ///< Estimable satellite phase bias (m); carries the pivot-receiver ambiguity lump (Odijk Eq. 4).
    bool   signal_integer{false}; ///< Bias preserves the integer ambiguity property (IGS-SSR).
    int    wide_lane_integer{0};  ///< Wide-lane integer indicator (2-bit, IGS-SSR).
};

/// One satellite's estimated satellite-side solution for a single epoch.
struct NetworkSatSolution {
    std::string                             sat;            ///< RINEX sat ID ("G01", "E11").
    bool                                    has_clock{false};  ///< Whether @c clock_c0_m is valid.
    double                                  clock_c0_m{0.0}; ///< Estimable satellite clock C0 (m); ZOH (c1=c2=0) for v1.
    std::map<std::string, double>           code_bias_m;    ///< RINEX obs code -> satellite code bias (m).
    std::map<std::string, NetworkPhaseBias> phase_bias;     ///< RINEX obs code -> satellite phase bias.
};

/// One epoch of the network joint solution to serialize.
struct NetworkSolutionEpoch {
    GnssTime                        epoch_time{0, 0.0}; ///< SSR content reference epoch.
    int                             iod_ssr{0};         ///< IOD-SSR stamped on every record this epoch.
    int                             provider_id{0};     ///< IGS-SSR provider id.
    int                             solution_id{0};     ///< IGS-SSR solution id.
    std::vector<NetworkSatSolution> sats;               ///< Per-satellite solutions.
};

/**
 * @class NetworkProductEmitter
 * @brief Serializes a @c NetworkSolutionEpoch into a @c SsrCorrectionStore.
 */
class NetworkProductEmitter {
public:
    /// @param disc_threshold_m Phase-bias jump (m) that counts as a discontinuity.
    ///        Default 0.05 m — well below a GPS L1 cycle (~0.19 m) so genuine
    ///        cycle-level bias jumps trip it while noise does not.
    explicit NetworkProductEmitter(double disc_threshold_m = 0.05);

    /// Serialize @p sol into @p store (Sink A — in-process, no wire bytes).
    void emitToStore(const NetworkSolutionEpoch& sol, SsrCorrectionStore& store);

    /// Force a discontinuity bump on every (sat, signal) emitted at the NEXT
    /// emit — e.g. on a global pivot handover / S-basis re-declaration (W6b).
    void markDatumChange();

    /// Current discontinuity counter for a (sat, signal), or 0 if untracked.
    int discontinuityCounter(const std::string& sat, const std::string& signal) const;

private:
    double disc_threshold_m_;
    bool   force_datum_bump_{false};
    std::map<std::string, int>    disc_counter_;    ///< "sat|signal" -> discontinuity counter.
    std::map<std::string, double> last_phase_bias_; ///< "sat|signal" -> last emitted bias (m).
};

} // namespace GnssProcessor
