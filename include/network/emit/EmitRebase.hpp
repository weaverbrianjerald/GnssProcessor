#pragma once
/**
 * @file EmitRebase.hpp
 * @brief Emit-layer frame re-basing of the network's per-(satellite,signal) phase
 *        biases onto the previous emission frame (Card 4 (M1) phase D).
 *
 * Under the invariant DD carry (`network.carry_invariant`,
 * docs/SPEC_card4_invariant_carry.md §D4 = its §6) the per-epoch eq34 datum is
 * re-realized freely: the spanning tree and the datum receiver r1 are re-elected
 * every epoch and the realized frame has no cross-epoch meaning (continuity lives
 * entirely in the carried double differences, which are S-system invariants).
 * That is correct for the filter, but the emitted product stream is a
 * per-satellite S-basis realization: the emitted @f$b_s@f$ carries the datum
 * receiver's ambiguity lump (Odijk Eq. 4), so a free re-realization steps every
 * satellite's emitted bias by a gauge element
 *
 *   @f[ b_s \;\to\; b_s + \delta + n_s\,\lambda, @f]
 *
 * with @f$\delta@f$ a common fractional per connected component (the new datum
 * receiver's fractional phase bias, which cancels in every between-satellite
 * difference) and @f$n_s@f$ a per-satellite integer (the chain-integer
 * reshuffle, which a user's integer DD ambiguity absorbs, so the fractional DD
 * that AR needs is untouched).  Both leave every user double difference invariant.
 *
 * Untreated, that per-epoch gauge churn would trip the emitter's 0.05 m
 * phase-bias jump test on nearly every satellite every epoch and cause spurious
 * ambiguity resets at the consumer.  This class removes it by an emit-layer
 * S-selection applied to the outputs only: after each accepted solve (before
 * @ref NetworkProductEmitter::emitToStore) it re-bases the emitted biases onto
 * the previous emission frame, so the emitted @f$b_s@f$ stays continuous across
 * pure datum churn and the emitter's jump test, which remains the sole
 * discontinuity authority, fires only at genuine events.
 *
 * ## Genuine events are preserved
 *
 * Aligning each key to its last emitted value would drive
 * @f$|b_s^{\text{emit}} - b_s^{\text{last}}|\to 0@f$ for every cause, including a
 * real cycle slip on the emission-reference (spanning-tree) path, which is the
 * quantity the jump test measures.  That would hide the slip (standing
 * invariant #1: a stale or blindly continued time-linked quantity past a slip
 * warps the whole solution).  This re-base therefore:
 *
 *   1. removes only the common per-component fractional @f$\delta@f$
 *      (estimated robustly by a median coordinate descent over the clean,
 *      baselined keys, so a minority of slipped/reattached satellites cannot
 *      drag it), and
 *   2. removes the per-satellite integer only for keys the caller has not
 *      flagged @ref Entry::slipped.  A slip on a satellite's emission-reference
 *      (tree-edge) arc leaves its integer step intact, so the emitter's jump
 *      test fires and the consumer resets that one ambiguity.  A first-sighting
 *      key (no baseline) also keeps @f$k_s=0@f$ (its birth is a genuine event;
 *      the emitter records it and the disc counter stays silent, as for any
 *      first sighting).
 *
 * ## Replay safety
 *
 * The re-base rewrites the emitted @c NetworkSolutionEpoch in place before the
 * store push and before the on-disk product write, so the serialized product
 * carries the re-based values.  A `NetworkProductIO` replay therefore reproduces
 * the emitter's discontinuity counters byte-for-byte without rerunning the
 * re-base: the transform is part of the product, as the eq34 anchor-handover
 * step is (ApplicationNetwork's "disc handled by the emitter bias-jump test" seam).
 *
 * ## Scope / conventions
 *
 * Pure numeric leaf; no dependency on the estimator or ApplicationNetwork.  The
 * caller resolves each emitted (sat,signal) to its connected-component id (a
 * union-find over the epoch's (receiver,satellite) graph in production; hand-
 * assigned in the gate) and its reference-path slip flag (from the network's
 * authoritative slip funnel), and supplies the signal wavelength.  The caller
 * must exclude GLONASS (FDMA, so no common integer @f$\lambda@f$): it is not
 * passed in and keeps its raw emitted bias.  With the feature off the output is
 * byte-identical by construction: the object is only constructed and called under
 * `network.carry_invariant && network.carry_invariant_emit_rebase`.
 */

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
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
        int         component{0};  ///< Connected-component id of the (const,signal) (rx,sat) graph.  Entries sharing a component share one common fractional @f$\delta@f$.  Caller-assigned (@ref networkEmitRebaseComponents in production; by hand in the gate).
        double      lambda_m{0.0}; ///< Carrier wavelength for @c signal (m), the integer re-base quantum.  The component uses the first positive @c lambda_m among its entries; when none is positive, the component is not re-based (@f$k_s=0@f$ and @f$\delta=0@f$).
        double      bias_m{0.0};   ///< In: raw emitted satellite phase bias (m).  Out: the re-based value the emitter/product must carry.
        bool        slipped{false};///< The emission-reference (spanning-tree) arc of this (sat,signal) slipped this epoch (from the slip funnel).  Its integer step is a genuine event and is left intact (@f$k_s=0@f$) so the emitter jump test fires.
    };

    /// Running tallies for the phase-D run-end telemetry line (spec §3).
    struct Counters {
        long long epochs{0};              ///< @ref rebaseEpoch invocations with at least one entry.
        long long components{0};          ///< components re-based, summed over epochs.
        long long integer_cycles{0};      ///< @f$\sum|k_s|@f$: integer cycles removed onto the previous frame.
        long long keys_rebased{0};        ///< (sat,signal) keys given a nonzero integer re-base.
        long long slips_preserved{0};     ///< slipped, baselined reference-path keys passed through (integer step left intact).
        double    frac_shift_abs_sum{0.0};///< @f$\sum|\delta|@f$ common-fractional shift over components (m); mean = this / @ref frac_shift_n.
        long long frac_shift_n{0};        ///< components processed (mean denominator); equals @c components, and a component without a clean key contributes @f$\delta=0@f$.
    };

    /**
     * @brief Re-base one epoch's emitted biases in place.
     *
     * Groups @p entries by @ref Entry::component; per component estimates the
     * common fractional @f$\delta@f$ (robust median coordinate descent over the
     * clean, baselined keys) and the per-key integer @f$k_s@f$, then rewrites
     * @f$b_s := b_s - k_s\lambda - \delta@f$.  Slipped or first-sighting keys
     * keep @f$k_s=0@f$.  Updates the internal emission-frame memory for every
     * key present this epoch, so the next epoch re-bases against these values,
     * which are what @ref NetworkProductEmitter stores as @c last_phase_bias_.
     *
     * @param entries The epoch's re-basable phase biases (GLONASS excluded by the
     *        caller).  @c bias_m is read then overwritten with the re-based value.
     */
    void rebaseEpoch(std::vector<Entry>& entries);

    const Counters& counters() const { return counters_; }

    /**
     * @brief The engine's cross-epoch emission frame, exportable for the RTB-7
     *        snapshot (RS-5 emit-layer continuity, docs/SPEC_event_harness.md
     *        §21.10): the previous emitted value of every "sat|signal" key.
     *
     * The engine is a run-local object, so without the export every warm resume
     * cold-starts it: the first post-resume epoch finds no baseline for any key,
     * re-bases nothing (@f$k_s=0@f$, @f$\delta=0@f$) and emits the raw solve-frame
     * value, while the continuous run's wire at the same epoch carries the whole
     * accumulated re-base.  The emitted product then steps by that accumulation,
     * which the emit layer alone accounts for (measurement in §21.10).  The CLK-R
     * engine has the same structure and the same fix,
     * @c NetworkClockEmitRebase::exportFrameState.
     *
     * @return "sat|signal" -> previous re-based emitted bias (m).  Counters are
     *         run telemetry, not frame state, and are not carried.
     */
    std::map<std::string, double> exportState() const { return last_emit_; }

    /**
     * @brief Restore a previously exported emission frame (a warm resume that
     *        continues the re-base instead of cold-starting it).
     *
     * Replaces the memory wholesale (a resume's previous emitted epoch is the
     * writer's last emitted epoch) and leaves @ref counters() alone, so the run's
     * own re-base telemetry counts only what this run did.  A key absent from
     * @p st is a first sighting, as it is on a continuous run.
     *
     * @param st Map exported by @ref exportState (via the snapshot's @c ER rows).
     */
    void restoreState(const std::map<std::string, double>& st) { last_emit_ = st; }

    /// Drop the emission-frame memory (a `carry_invariant` toggle-flip cold start;
    /// next epoch every key re-bases against nothing, a one-time first sighting).
    void reset();

    /// The last re-based emitted value for (sat,signal) (test seam); false if the
    /// key has no emission-frame memory yet.
    bool lastEmittedForTest(const std::string& sat, const std::string& signal,
                            double& value) const;

private:
    /// "sat|signal" -> last re-based emitted bias (m): the previous emission frame.
    std::map<std::string, double> last_emit_;
    Counters counters_;
};

/**
 * @brief The emit re-base graph of one epoch: per (constellation letter, phase signal)
 *        group, the shared (receiver, satellite) arcs as (station index, satellite id),
 *        in the caller's walk order.  Station indices are non-negative.
 */
using EmitRebaseArcs =
    std::map<std::pair<char, std::string>, std::vector<std::pair<int, std::string>>>;

/**
 * @brief The connected-component id of every (satellite, signal) of the emit re-base
 *        graph: the @ref NetworkEmitRebase::Entry::component the re-base groups by.
 *
 * Per group the arcs are united on the shared union-find (@c arcgraph::Dsu,
 * network/ar/ArcGraph.hpp, card AGT): station index i is vertex i, the satellite of
 * lexical rank k is the vertex after every station's plus k, and each arc links the
 * station under the satellite, in arc order.  Ids are 1-based and global across groups:
 * groups in key order, and within a group the components numbered in the order of
 * their lexically first satellite.  Every id therefore depends on the partition alone,
 * neither on the arc order nor on which vertex is a root.
 *
 * @param arcs The epoch's re-base graph (the caller has already excluded GLONASS and
 *             every key outside the eq34 graph).
 * @return "sat|signal" -> component id.  A (satellite, signal) with no arc is absent.
 */
std::map<std::string, int> networkEmitRebaseComponents(const EmitRebaseArcs& arcs);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the emit-layer frame re-base.
 *
 * The emit-layer frame re-base of the emitted phase biases (card 4/M1 phase D).
 *
 * Gate: returns 0 at once unless `emit_rebase_on`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkEmitRebaseRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
