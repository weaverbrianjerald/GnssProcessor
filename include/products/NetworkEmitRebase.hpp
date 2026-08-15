#pragma once
/**
 * @file NetworkEmitRebase.hpp
 * @brief Card 4 (M1) phase D — emit-layer frame re-basing of the network's
 *        per-(satellite,signal) phase biases onto the previous EMISSION frame.
 *
 * Under the invariant DD carry (`network.carry_invariant`,
 * docs/SPEC_card4_invariant_carry.md §D4 = its §6) the per-epoch eq34 datum is
 * re-realized *freely* — the
 * spanning tree / datum receiver r1 are re-elected every epoch and NO cross-epoch
 * meaning attaches to the realized frame (continuity lives entirely in the carried
 * double differences, which are S-system invariants).  That is correct for the
 * FILTER, but the emitted product stream is a per-satellite S-basis realization:
 * the emitted @f$b_s@f$ carries the datum receiver's ambiguity lump (Odijk Eq. 4),
 * so a free re-realization steps every satellite's emitted bias by a gauge element
 *
 *   @f[ b_s \;\to\; b_s + \delta + n_s\,\lambda, @f]
 *
 * with @f$\delta@f$ a **common per-connected-component fractional** (the new
 * datum receiver's fractional phase bias — cancels in every between-satellite
 * difference) and @f$n_s@f$ a **per-satellite integer** (the chain-integer
 * reshuffle — a user's integer DD ambiguity absorbs it, so the fractional DD that
 * AR needs is untouched).  Both leave every user double difference invariant.
 *
 * Left un-treated, that per-epoch gauge churn would trip the emitter's 0.05 m
 * phase-bias jump test on nearly every satellite every epoch and storm the
 * consumer with spurious ambiguity resets.  This class is the emit-layer
 * **S-selection on the OUTPUTS ONLY** that removes it: after each accepted solve
 * (before @ref NetworkProductEmitter::emitToStore) it re-bases the emitted
 * biases onto the previous emission frame, so the emitted @f$b_s@f$ stays
 * continuous across pure datum churn and the emitter's jump test — which remains
 * the sole discontinuity authority — fires only at GENUINE events.
 *
 * ## The one non-negotiable: never launder a genuine event
 *
 * A blind "align each key to its last emitted value" would drive
 * @f$|b_s^{\text{emit}} - b_s^{\text{last}}|\to 0@f$ for EVERY cause — including a
 * real cycle slip on the emission-reference (spanning-tree) path, which is
 * *exactly* the quantity the jump test measures.  That is definitionally a
 * slip-laundering operation (standing invariant #1: a stale/blindly-continued
 * time-linked quantity past a slip warps the whole solution).  This re-base
 * therefore:
 *
 *   1. removes only the **common per-component fractional** @f$\delta@f$
 *      (estimated robustly — a median coordinate descent over the CLEAN,
 *      baselined keys, so a minority of slipped/reattached satellites cannot
 *      drag it), and
 *   2. removes the **per-satellite integer** only for keys the caller has NOT
 *      flagged @ref Entry::slipped.  A slip on a satellite's emission-reference
 *      (tree-edge) arc leaves its integer step INTACT, so the emitter's jump
 *      test fires and the consumer resets that one ambiguity.  A first-sighting
 *      key (no baseline) likewise keeps @f$k_s=0@f$ (its birth is a genuine
 *      event; the emitter records it and the disc stays silent as first sight).
 *
 * ## Replay safety
 *
 * The re-base rewrites the emitted @c NetworkSolutionEpoch IN PLACE before the
 * store push AND before the on-disk product write, so the serialized product
 * carries the re-based values.  A `NetworkProductIO` replay therefore reproduces
 * the emitter's discontinuity counters byte-for-byte with NO re-base rerun — the
 * transform is baked into the product, exactly as the eq34 anchor-handover step
 * is (ApplicationNetwork's "disc handled by the emitter bias-jump test" seam).
 *
 * ## Scope / conventions
 *
 * Pure numeric leaf; no dependency on the estimator or ApplicationNetwork.  The
 * caller resolves each emitted (sat,signal) to its connected-component id (a
 * union-find over the epoch's (receiver,satellite) graph in production; hand-
 * assigned in the gate) and its reference-path slip flag (from the network's
 * authoritative slip funnel), and supplies the signal wavelength.  GLONASS
 * (FDMA — no common integer @f$\lambda@f$) must be excluded by the caller (it is
 * simply not passed in and keeps its raw emitted bias).  Byte-identity OFF is
 * structural: the object is only constructed / called under
 * `network.carry_invariant && network.carry_invariant_emit_rebase`.
 */

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class NetworkEmitRebase
 * @brief Re-bases a network solution epoch's per-(sat,signal) phase biases onto
 *        the previous emission frame (integer + common-fractional S-selection).
 */
class NetworkEmitRebase {
public:
    /// One emitted (sat,signal) phase bias to re-base this epoch.
    struct Entry {
        std::string sat;           ///< RINEX satellite id ("G01", "E11").
        std::string signal;        ///< RINEX phase obs code ("L1C").
        int         component{0};  ///< Connected-component id of the (const,signal) (rx,sat) graph.  Entries sharing a component share ONE common fractional @f$\delta@f$.  Caller-assigned (union-find in production; by hand in the gate).
        double      lambda_m{0.0}; ///< Carrier wavelength for @c signal (m) — the integer re-base quantum.  <=0 disables the integer step for this entry (fractional-only).
        double      bias_m{0.0};   ///< IN: raw emitted satellite phase bias (m).  OUT: the re-based value the emitter/product must carry.
        bool        slipped{false};///< The emission-reference (spanning-tree) arc of this (sat,signal) slipped this epoch (from the slip funnel).  Its integer step is a GENUINE event — left intact (@f$k_s=0@f$) so the emitter jump test fires; NEVER laundered.
    };

    /// Running tallies for the phase-D run-end telemetry line (spec §3).
    struct Counters {
        long long epochs{0};              ///< @ref rebaseEpoch invocations that re-based >=1 entry.
        long long components{0};          ///< components re-based, summed over epochs.
        long long integer_cycles{0};      ///< @f$\sum|k_s|@f$ — integer cycles removed onto the previous frame.
        long long keys_rebased{0};        ///< (sat,signal) keys given a NONZERO integer re-base.
        long long slips_preserved{0};     ///< slipped reference-path keys passed through (integer step left intact).
        double    frac_shift_abs_sum{0.0};///< @f$\sum|\delta|@f$ common-fractional shift over components (m) — mean = /@ref frac_shift_n.
        long long frac_shift_n{0};        ///< components contributing a @f$\delta@f$ (mean denominator).
    };

    /**
     * @brief Re-base one epoch's emitted biases IN PLACE.
     *
     * Groups @p entries by @ref Entry::component; per component estimates the
     * common fractional @f$\delta@f$ (robust median coordinate descent over the
     * clean, baselined keys) and the per-key integer @f$k_s@f$, then rewrites
     * @f$b_s := b_s - k_s\lambda - \delta@f$.  Slipped or first-sighting keys
     * keep @f$k_s=0@f$.  Updates the internal emission-frame memory for every
     * key present this epoch, so the next epoch re-bases against these values —
     * exactly what @ref NetworkProductEmitter stores as @c last_phase_bias_.
     *
     * @param entries The epoch's re-basable phase biases (GLONASS excluded by the
     *        caller).  @c bias_m is read then overwritten with the re-based value.
     */
    void rebaseEpoch(std::vector<Entry>& entries);

    const Counters& counters() const { return counters_; }

    /// Drop the emission-frame memory (a `carry_invariant` toggle-flip cold start;
    /// every key re-bases against nothing — a one-time first sighting — next epoch).
    void reset();

    /// The last re-based emitted value for (sat,signal) (test seam); false if the
    /// key has no emission-frame memory yet.
    bool lastEmittedForTest(const std::string& sat, const std::string& signal,
                            double& value) const;

private:
    /// "sat|signal" -> last re-based emitted bias (m) — the previous emission frame.
    std::map<std::string, double> last_emit_;
    Counters counters_;
};

}  // namespace GnssProcessor
