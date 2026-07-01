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
 * It owns the analogous bookkeeping for satellite CODE biases (H1): the
 * per-(constellation, signal) sum-to-zero datum re-references whenever the
 * satellite SET carrying a signal changes (rise/set), and the per-satellite
 * ionosphere-free pin re-references whenever that satellite's distinct-band set
 * changes — both step the emitted code biases.  The emitter detects both from
 * the epoch's own @c code_bias_m composition (so replay reproduces the counters
 * with NO new serialization, exactly as for phase), and also honours the same
 * @c markDatumChange forced bump (a clock-pivot handover re-references the whole
 * shared solution).  There is no magnitude threshold for code: the deterministic
 * set/band-change detectors cover every gauge re-reference, and a code RW noise
 * step is not a datum discontinuity.  The consumer's @c getCodeBiasDiscontinuity
 * -> @c reinflateCodeBiasVariance hook turns an increment into a clean receiver
 * code-bias re-inflation.
 *
 * Sink B (RTCM-4076 wire encoding) is deferred to Phase 2.
 *
 * NOTE: push into an OWNED (non-singleton) SsrCorrectionStore for the closed
 * loop — a producer must never share the store it (or its consumer) reads from
 * the global singleton, and tests must not pollute global state.
 */

#include "rtcm/RtcmTypes.hpp"   // GnssTime, SSR record structs

#include <map>
#include <set>
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

    /// Current phase-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    int discontinuityCounter(const std::string& sat, const std::string& signal) const;

    /// Current code-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    int codeBiasDiscontinuityCounter(const std::string& sat, const std::string& signal) const;

private:
    double disc_threshold_m_;
    bool   force_datum_bump_{false};
    std::map<std::string, int>    disc_counter_;    ///< "sat|signal" -> phase discontinuity counter.
    std::map<std::string, double> last_phase_bias_; ///< "sat|signal" -> last emitted phase bias (m).

    // --- Satellite code-bias discontinuity bookkeeping (H1) ---------------
    // prev_code_satset_ / prev_band_tuple_ are WHOLESALE-REPLACED with the
    // current epoch's maps inside emitToStore (never merged): a (const,signal)
    // channel or a satellite that disappears and later reappears is then a
    // clean first-sighting on both a live run AND a fresh replay process, so
    // the counters reproduce deterministically.  They update ONLY on an actual
    // emit (the writer persists only emitted epochs; replay must reproduce the
    // emitted-only diff sequence).
    std::map<std::string, int>    code_disc_counter_; ///< "sat|signal" -> code discontinuity counter.
    std::map<std::string, double> last_code_bias_;    ///< "sat|signal" -> last emitted code bias (m); first-sighting marker.
    std::map<std::string, std::set<std::string>> prev_code_satset_; ///< "const|signal" -> sats carrying it last emit.
    std::map<std::string, std::vector<char>>     prev_band_tuple_;  ///< "sat" -> sorted distinct code band-digits last emit (IF-pin proxy).
};

} // namespace GnssProcessor
