/**
 * @file NetworkAmbFloat.hpp
 * @brief Float ambiguity vehicle: the floored birth prior and the bounded drop that
 *        replace the leveling vehicle's reset paths (RF-1 step 4).
 *
 * Design: docs/refound/RF-1_float_ambiguities_design.md §3.3 / §3.4 / §7.2,
 * pre-registered in docs/refound/RF-1_steps12_read_2026-09-15.md.  Active only under
 * @c network.ambiguity_vehicle = "float_kf".
 *
 * Leveling-vehicle behaviour replaced here (design §1.5).  Four paths return a carried
 * ambiguity to a level re-determined from one epoch without restoring a birth-scale
 * variance: the free-epoch latch and the gap cliff skip one inject and let the harvest
 * re-stash the level at a phase-stiff variance; the T4 and T3 refloats delete the entry.
 * The successor arc then starts orders of magnitude looser than the prior it replaced
 * and needs many folds to recover (design §1.5 gives the field figures).
 *
 * The replacement is one operator (§3.4).  A slip reduces confidence in the level and
 * does not change the arc's identity, so a drop restores the birth prior and keeps
 * everything else:
 *
 *     lc.var       = max(var_birth, var_first, floor(elevation))
 *     lc.coupling.clear();  lc.blk = nullptr;  lc.blk_idx = -1
 *     lc.relevel_s = now;   lc.dropped_ep = now_ep;  lc.drop_walk_ph = 1
 *     // value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s, n_folds kept
 *
 * The prior is still folded in the same epoch, at birth strength.  Consequences
 * (standing invariant #7):
 *  1. The decoupling edge is bounded.  A free epoch removes all information about the
 *     column for one epoch, opening an unbounded direction that the shared parameters
 *     re-balance into (card R10 ties unsignalled satellite-clock steps to T4 refloats of
 *     the same satellite).  A drop removes information down to a stated level, so the
 *     response is proportional.
 *  2. The re-coupling edge re-validates by construction: the arc re-earns its variance
 *     as information accumulates, visible in the @c fold_var trajectory of
 *     @c network_amb_state.csv.
 *  3. @c value_abs survives (design decision of 2026-09-14, question 2).  It is the
 *     linearization point, so keeping it is an exact solve no-op, and the
 *     lane-preserving refloat depends on it.
 *
 * Cases that remain an erase: the gap prune past @c phase_leveling_max_gap_s and an
 * explicit re-admission beyond the readmit horizon, the one case that means "this is a
 * different arc" (§3.4).  Both remain events with both edges counted.
 */
#pragma once

#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkCarrySites.hpp"   // CarrySiteCounts (card PRM phase 2)

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;
    struct NetStation;

    /// Cause tags for a drop: the @c event vocabulary of @c network_amb_state.csv and
    /// the key of the run-end inventory.  One constant per cause, shared by the writer
    /// and the reader.
    namespace AmbDropCause {
        inline constexpr const char* kReLevelFree = "relevel_free";  ///< a detector's latched free-epoch set (the five NetworkSlipQc sites).
        inline constexpr const char* kStaleGap    = "stale_gap";     ///< the inject-site gap cliff (gap > phase_leveling_max_gap_s, arc still present).
        inline constexpr const char* kT4Attrib    = "t4_attrib";     ///< RAIM prominence-attribution refloat (an erase on the leveling vehicle).
        inline constexpr const char* kT3Consensus = "t3_consensus";  ///< RAIM per-satellite consensus refloat (an erase on the leveling vehicle).
    }

    /**
     * @brief Birth-prior floor at one line of sight (m²); design §3.3(b), elevation-mapped.
     *
     * The floor states what a code-determined level is worth, so it is mapped the same
     * way as the code row weights: @f$\sigma^2(e)=\sigma^2_\text{zen}/
     * \sin^{2p}(e)@f$ with @p elev_power the posture's @c filter.code_elevation_power.
     * A flat zenith floor would be optimistic at the low elevations that hold the
     * metre-plus tail (§24.13 finding 2: 25.8 % of iono rows lie below the tie cutoff).
     * The sine is clamped at sin(5°), so the floor is bounded for grazing elevations.
     *
     * @param base_m2    @c network.amb_float_birth_var_m2, the floor at zenith (m²).
     *                   A value <= 0 disables the floor (returns 0).
     * @param elev_deg   LOS elevation (degrees).  A non-finite or non-positive value
     *                   (elevation unavailable) returns the zenith value, the smallest
     *                   the floor can be, so it never adds confidence the arc has not
     *                   earned.
     * @param elev_power The σ-power of the code weights (@c getCodeElevationPower()).
     * @return The floor in m² at that elevation, clamped at the zenith value from below.
     */
    double ambFloatBirthFloor(double base_m2, double elev_deg, double elev_power);

    /**
     * @brief Apply the drop operator to one carried entry, in place (§3.4).
     *
     * @param lc        The entry.  @c value_abs, @c var_first, @c var_birth, both birth
     *                  stamps, @c last_seen_s and @c n_folds are untouched.
     * @param floor_m2  This LOS's birth floor from @ref ambFloatBirthFloor; the restored
     *                  variance is @c max(var_birth, var_first, floor_m2), so a drop can
     *                  only loosen the prior, never tighten it.
     * @param now_s     Continuous-GPS seconds, the new @c relevel_s (the age clock
     *                  restarts, as on a re-level).
     * @param now_ep    This epoch's @c n_loop_epochs, the new @c dropped_ep.
     * @return True when the entry changed (its variance was tighter than the birth
     *         prior).  False when the arc was already at or below birth strength (an
     *         idempotent re-drop within one epoch, counted separately; @c relevel_s,
     *         @c dropped_ep and @c drop_walk_ph are still updated), or when the
     *         restored variance would not be positive (the entry is left unchanged).
     */
    bool ambFloatDrop(LocalCarry& lc, double floor_m2, double now_s, long long now_ep,
                      bool decouple_always = false, bool* decoupled_idem = nullptr);
    // @p decouple_always (network.amb_float_drop_decouple_always, card CTU section 14):
    // the idempotent branch also clears the border regression and the block row when the
    // arc still has them, and sets *@p decoupled_idem.  Default false = the previous
    // behaviour.

    /**
     * @brief Outcome of a drop taken with both floors in hand (RF-3a A1).
     *
     * @c from_drop_floor is the drop floor's own ON-path telemetry (standing invariant
     * #4): it is true when the drop-only floor determined the restored variance, i.e. it
     * exceeded the arc's own birth prior and the birth floor, and the variance moved.
     * A count of 0 under an armed drop floor means every dropped arc was already looser
     * than the floor; that is a reading, not an inert run.
     */
    struct AmbDropOutcome {
        bool acted{false};            ///< The entry's variance actually moved (@ref ambFloatDrop's return).
        bool from_drop_floor{false};  ///< The restored variance came from @c network.amb_float_drop_var_m2.
        bool decoupled_idem{false};   ///< Card CTU section 14: the variance did not move but the regression and the block row were cleared (network.amb_float_drop_decouple_always).
    };

    /**
     * @brief Drop taken at both floors; the one function all four drop causes use, so
     *        they cannot diverge (RF-3a A1).
     *
     * The restored variance is @c max(var_birth, var_first, birth_floor, drop_floor),
     * with both floors already elevation-mapped by @ref ambFloatBirthFloor at the call
     * site (the code weights' own mapping, so a floor stated at zenith is not
     * optimistic at the low elevations that hold the metre-plus tail).  The birth stash
     * does not use @p drop_floor_m2.  The RF-3 read measured that a floor at birth
     * removes the post-drop walk but also slows every birth, while the defect this knob
     * targets is the re-coupling edge of the slip and gap re-entry response.
     *
     * @param lc            The entry.
     * @param birth_floor_m2 This LOS's @c network.amb_float_birth_var_m2 floor (m²).
     * @param drop_floor_m2  This LOS's @c network.amb_float_drop_var_m2 floor (m²).
     * @param now_s         Continuous-GPS seconds, the new @c relevel_s.
     * @param now_ep        This epoch's @c n_loop_epochs, the new @c dropped_ep.
     * @return the outcome.
     */
    AmbDropOutcome ambFloatDropFloored(LocalCarry& lc, double birth_floor_m2,
                                       double drop_floor_m2, double now_s,
                                       long long now_ep, bool decouple_always = false);

    /**
     * @brief Drop floor in force at this epoch, i.e. the warm-up gate (RF-3c).
     *
     * Shared by the drop sites (this file's free-epoch latch and gap cliff, and the
     * RAIM refloats in NetworkRaim.cpp), so every site reads the gate the same way.
     * Returns 0 for a drop taken before the gate opens, which leaves the restored
     * variance at @c max(var_birth, var_first, birth floor), and the floor itself from
     * epoch @p after_ep onwards.  Returns 0 when @p drop_floor_m2 <= 0.
     *
     * The abscissa is the producer's processed-epoch counter, the one
     * @c network.gross_los_warmup_epochs and the phase-leveling warmup read, because
     * the gate draws the same distinction they do: an arc dropped while the network is
     * still converging its clocks is a convergence event, and the worth of a
     * re-determined level is a property of a settled network.
     *
     * @param drop_floor_m2 @c network.amb_float_drop_var_m2 at this line of sight (m²).
     * @param now_ep        This epoch's @c n_loop_epochs.
     * @param after_ep      @c network.amb_float_drop_floor_after_ep; <= 0 = no gate.
     * @return the floor to use at this drop (m²).
     */
    double ambFloatDropFloorNow(double drop_floor_m2, long long now_ep, int after_ep);

    /**
     * @brief Serial pre-assembly drop pass: the free-epoch latch and the gap cliff.
     *
     * Runs after PASS A's slip scans (which fill @c ctx.carry.relevel.free_ambs) and
     * before the PASS-B assembly, so when the parallel inject reads an entry the drop
     * has already happened and the inject folds it at birth strength instead of
     * skipping it.  Serial because it mutates @c ctx.carry.local_carry, which the
     * produce phase then reads.
     *
     * Two causes, both of which skip an inject on the leveling vehicle:
     *  - @c relevel_free: every @c (station, amb key) the detectors latched this epoch.
     *    The latched set itself is left in place, because the lane observatory, the
     *    phase-datum candidacy screen, the anchor-slip handover staging and the
     *    harvest's statistics guards all read it (§4.1: "keep the latched-set
     *    plumbing").  With @c network.phase_datum_held_basis armed, every latched arc
     *    is also stamped in @c ctx.carry.amb_flag_ep with this epoch, whether or not it
     *    has a carry (RF-2 items 3 and 4).
     *  - @c stale_gap: every carried arc whose sighting gap already exceeds
     *    @c phase_leveling_max_gap_s.  The post-harvest prune still erases arcs that are
     *    gone; this pass targets arcs present this epoch, which the prune cannot see
     *    because the harvest refreshes @c last_seen_s first.  The loop does not test
     *    presence, so an absent arc past the horizon is also dropped and counted here
     *    before the prune erases it.
     *
     * A no-op unless the vehicle is @c float_kf.
     *
     * @param ctx       The run context (the carry, the knobs, the counters, the trace buffer).
     * @param config_   The run config (the code elevation power).
     * @param log       The network logger.
     * @param stations  The station roster, for the trace rows' station names.
     * @param elev_of   Per station, this epoch's LOS elevation by satellite (degrees);
     *                  empty when the birth floor is unarmed, in which case both floors
     *                  use their zenith value and the pass costs one map walk.
     * @param week      GPS week of this epoch (the trace rows' stamp).
     * @param tow       GPS time of week (s) of this epoch.
     * @param pl_now_s  Continuous-GPS seconds of this epoch.
     */
    void networkAmbFloatApplyDrops(
        NetworkRunContext& ctx, const GnssConfig& config_,
        const std::shared_ptr<spdlog::logger>& log,
        const std::vector<NetStation>& stations,
        const std::vector<std::map<std::string, double>>& elev_of,
        long long week, double tow, double pl_now_s);

    /**
     * @brief The declared anchor handover's transform of one (satellite, signal) family
     *        through the traversal of carried references (card PRM phase 2,
     *        @c CarryMechanism::DeclaredHandover).
     *
     * Every carried ambiguity of the family moves by @f$-\Delta@f$, the satellite's carried
     * phase bias of that signal by @f$+\Delta@f$, and the s_prev of every phase-bias coupling
     * term on it in the ambiguity carry by @f$+\Delta@f$ (variances untouched: an exact basis
     * change).  The sites the transform does not move are declared with their reason in the
     * mechanism table.
     *
     * With @p complete (@c network.carry_rebase_complete, card PRM phase 3b) the transform
     * also moves the sites the table exempts as owed, each by the same rule: the gross
     * screen's held ambiguity of the family by @f$-\Delta@f$ (as the ambiguity), the last
     * solved phase bias (@c pb_level_prev) and an open pin bridge of the family ("SAT_SIGNAL")
     * by @f$+\Delta@f$ (as the carried phase bias), and the s_prev of every phase-bias
     * coupling term on the family's column in every other coupling map (the wet delay, the
     * slant ionosphere and its block, the gross screen's three holds, the receiver clock,
     * phase bias and code bias, card LGM's phase colour) by @f$+\Delta@f$.  The phase hold's
     * held value is an emitted value and is not moved.
     * @param carry    the run's carried state.
     * @param sat      the family's satellite.
     * @param sig      the family's signal.
     * @param delta    the incoming anchor's carried level (m), read before anything moves.
     * @param n_shift  out: carried ambiguities moved (the held ambiguity moved under
     *                 @p complete is counted in @p counts only).
     * @param n_pb     out: carried phase biases moved (the baseline and the bridge moved under
     *                 @p complete are counted in @p counts only).
     * @param n_cpl    out: coupling terms moved (every coupling map's).
     * @param counts   optional out: per-site tallies (accumulated).
     * @param complete also move the owed sites (false = the walk of card PRM phase 2).
     */
    void declaredHandoverShiftCarry(NetworkCarryState& carry, const std::string& sat,
                                    const std::string& sig, double delta,
                                    long long& n_shift, long long& n_pb, long long& n_cpl,
                                    CarrySiteCounts* counts = nullptr, bool complete = false);

    /**
     * @brief Declared anchor S-transform (design §3.6), replacing the network-wide
     *        erase of @c networkApplyPhaseAnchorCarryReref for continuous handovers
     *        (RF-1 step 5).
     *
     * Requires @c float_kf and @c network.amb_float_declared_handover.  Without it, a
     * (satellite, signal) family's re-reference drops every station's carry for that
     * family in one epoch, and a state erased at every anchor handover does not persist
     * as a state (design decision of 2026-09-14, question 3).  The frame does step, so
     * the carried values are stale in the old basis; but a frame step is a known, exact
     * basis change, so the carry is expressed in the new basis instead:
     *
     *     Δ = value_abs(r_in, sat, signal)          // the incoming anchor's carried level
     *     for every station r carrying (sat, signal):  value_abs_r −= Δ
     *     the shared phase bias of (sat, signal):      value      += Δ
     *     every coupling s_prev on that PB column:                += Δ
     *     // variances untouched: an exact basis change, not a refloat
     *
     * The incoming anchor's own column then reads 0, which its @f$w=10^{10}@f$ gauge pin
     * is about to assert.  That identity is checked against @c kStsExactTolM and
     * reported, never thrown (the @c sts_selfcheck_ok pattern of
     * @ref networkHandoverStransform).
     *
     * Two filed latent defects this function must not re-open:
     *  (i) The D4 carry S-transformation in @c NetworkChainDatum.cpp
     *      (SPEC_card1_coupling_carry §14) shifts @c lc.value_abs and every coupling
     *      @c s_prev on a shifted PB column together, because a Card-1 coupling row's
     *      RHS is @f$y=(v-x_{lin})+\sum_j M_j\hat s_{prev,j}@f$ and, for a
     *      gauge-invariant row under a known frame step, the two moves cancel exactly.
     *      Shifting one and not the other injects the frame step as row bias.  This
     *      function moves both, plus the shared PB carried prior, the third reference
     *      on the same identity.
     *  (ii) Invariant #7: the transform is an event with both edges.  One
     *      @c STRANSFORM row per family (@c sts_applied, or @c sts_selfcheck_fail when
     *      the self-check fails) carries the outgoing and incoming anchors, Δ, the
     *      number of carries shifted and the self-check residual.
     *
     * Scope.  Only a continuous handover has a Δ: the successor was elected with a
     * carried level, and that level is the new basis.  A cold re-realization has no
     * ledger entry for the successor, so its family keeps the erase; cold handovers,
     * an unknown incoming station, and a continuous handover whose incoming anchor
     * holds no live carry are counted separately as refused.  Δ = 0 is the identity:
     * the family is taken over without a shift.  The election policy that makes cold
     * handovers rare belongs to RF-2.
     *
     * @param ctx         The run context (the carry, the shared prior stash, the counters).
     * @param log         The network logger.
     * @param traces      The trace streams, for the @c STRANSFORM event rows.
     * @param stations    The station roster (resolving the incoming anchor's index).
     * @param handovers   This epoch's anchor handovers, from the election.
     * @param week        GPS week of the epoch.
     * @param tow         GPS time of week (s) of the epoch.
     * @param handled_out Receives every (sat, signal) family this transform took
     *                    ownership of (shifted or identity); the caller removes them from
     *                    the erase set, so a family is transformed or erased, never both.
     * @return The number of carried entries whose value was shifted.
     */
    long long networkAmbFloatDeclaredHandover(
        NetworkRunContext& ctx,
        const std::shared_ptr<spdlog::logger>& log,
        NetworkRunTraces& traces,
        const std::vector<NetStation>& stations,
        const std::vector<AnchorHandoverEvent>& handovers,
        long long week, double tow,
        std::set<std::pair<std::string, std::string>>& handled_out);

}  // namespace GnssProcessor
