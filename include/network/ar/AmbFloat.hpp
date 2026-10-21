/**
 * @file AmbFloat.hpp
 * @brief Float ambiguity vehicle: the floored birth prior and the bounded drop that
 *        replace the leveling vehicle's reset paths.
 *
 * Active only under @c network.ambiguity_vehicle = "float_kf".
 *
 * Leveling-vehicle behaviour replaced here.  Four paths return a carried
 * ambiguity to a level re-determined from one epoch without restoring a birth-scale
 * variance: the free-epoch latch and the gap cliff skip one inject and let the harvest
 * re-stash the level at a phase-stiff variance; the RAIM attribution and consensus
 * refloats delete the entry.
 * The successor arc then starts orders of magnitude looser than the prior it replaced
 * and needs many folds to recover.
 *
 * The replacement is one operator.  A slip reduces confidence in the level and
 * does not change the arc's identity, so a drop restores the birth prior and keeps
 * everything else:
 *
 *     lc.var       = max(var_birth, var_first, floor(elevation))
 *     lc.coupling.clear();  lc.blk = nullptr;  lc.blk_idx = -1
 *     lc.relevel_s = now;   lc.dropped_ep = now_ep;  lc.drop_walk_ph = 1
 *     // value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s, n_folds kept
 *
 * The prior is still folded in the same epoch, at birth strength.  Consequences:
 *  1. The decoupling edge is bounded.  A free epoch removes all information about the
 *     column for one epoch, opening an unbounded direction that the shared parameters
 *     re-balance into (unsignalled satellite-clock steps are tied to RAIM attribution
 *     refloats of the same satellite).  A drop removes information down to a stated
 *     level, so the response is proportional.
 *  2. The re-coupling edge re-validates by construction: the arc re-earns its variance
 *     as information accumulates, visible in the @c fold_var trajectory of
 *     @c network_amb_state.csv.
 *  3. @c value_abs survives: a prior at birth variance is dominated by the epoch's
 *     data, so keeping the carried level costs little.
 *
 * Cases that remain an erase: the gap prune past @c phase_leveling_max_gap_s and an
 * explicit re-admission beyond the readmit horizon, the one case that means "this is a
 * different arc".  Both remain events with both edges counted.
 */
#pragma once

#include "network/driver/RunTypes.hpp"
#include "network/state/CarrySites.hpp"   // CarrySiteCounts

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

    class GnssConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;
    struct NetStation;

    /// Cause tags for a drop: the @c event vocabulary of @c network_amb_state.csv and
    /// the key of the run-end inventory.  One constant per cause, shared by the writer
    /// and the reader.
    namespace AmbDropCause {
        inline constexpr const char* kReLevelFree = "relevel_free";  ///< a detector's latched free-epoch set (@c relevel.free_ambs).
        inline constexpr const char* kStaleGap    = "stale_gap";     ///< the inject-site gap cliff (gap > phase_leveling_max_gap_s).
        inline constexpr const char* kT4Attrib    = "t4_attrib";     ///< RAIM prominence-attribution refloat (an erase on the leveling vehicle).
        inline constexpr const char* kT3Consensus = "t3_consensus";  ///< RAIM per-satellite consensus refloat (an erase on the leveling vehicle).
    }

    /**
     * @brief Birth-prior floor at one line of sight (m²), elevation-mapped.
     *
     * The floor states what a code-determined level is worth, so it is mapped the same
     * way as the code row weights: @f$\sigma^2(e)=\sigma^2_\text{zen}/
     * \sin^{2p}(e)@f$ with @p elev_power the posture's @c filter.code_elevation_power.
     * A flat zenith floor would be optimistic at the low elevations that hold the
     * metre-plus tail.
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
     * @brief Apply the drop operator to one carried entry, in place.
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
    // @p decouple_always (network.amb_float_drop_decouple_always):
    // the idempotent branch also clears the border regression and the block row when the
    // arc still has them, and sets *@p decoupled_idem.  Default false leaves them in place.

    /**
     * @brief Outcome of a drop taken with both floors in hand.
     *
     * @c from_drop_floor is the drop floor's own telemetry: it is true when the drop-only
     * floor determined the restored variance, i.e. it exceeded the arc's own birth prior
     * and the birth floor, and the variance moved.  A count of 0 under an armed drop
     * floor means the floor never set a restored variance; that is a reading, not an
     * inert run.
     */
    struct AmbDropOutcome {
        bool acted{false};            ///< The entry's variance actually moved (@ref ambFloatDrop's return).
        bool from_drop_floor{false};  ///< The restored variance came from @c network.amb_float_drop_var_m2.
        bool decoupled_idem{false};   ///< The variance did not move but the regression and the block row were cleared (network.amb_float_drop_decouple_always).
    };

    /**
     * @brief Drop taken at both floors; the one function all four drop causes use, so
     *        they cannot diverge.
     *
     * The restored variance is @c max(var_birth, var_first, birth_floor, drop_floor),
     * with both floors already elevation-mapped by @ref ambFloatBirthFloor at the call
     * site (the code weights' own mapping, so a floor stated at zenith is not
     * optimistic at the low elevations that hold the metre-plus tail).  The birth stash
     * does not use @p drop_floor_m2.  A floor at birth was measured to remove the
     * post-drop walk but also to slow every birth, while the defect this knob targets
     * is the re-coupling edge of the slip and gap re-entry response.
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
     * @brief Drop floor in force at this epoch, i.e. the warm-up gate.
     *
     * Shared by the drop sites (this file's free-epoch latch and gap cliff, and the
     * RAIM refloats in Raim.cpp), so every site reads the gate the same way.
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
     * Runs after PASS A's slip scans (which fill @c events.slip.relevel.free_ambs) and
     * before the PASS-B assembly, so when the parallel inject reads an entry the drop
     * has already happened and the inject folds it at birth strength instead of
     * skipping it.  Serial because it mutates @c ctx.entities.arcs, which the
     * produce phase then reads.
     *
     * Two causes, both of which skip an inject on the leveling vehicle:
     *  - @c relevel_free: every @c (station, amb key) the detectors latched this epoch.
     *    The latched set itself is left in place, because the
     *    phase-datum candidacy screen, the anchor-slip handover staging and the
     *    harvest's statistics guards all read it.  With @c network.phase_datum_held_basis
     *    armed, every latched arc is also stamped in the station table's @c amb_flag_ep
     *    with this epoch, whether or not it has a carry.
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
     * @param events The epoch's events; writes slip (the relevel free-epoch latch).
     */
    void networkAmbFloatApplyDrops(
        NetworkRunContext& ctx, const GnssConfig& config_,
        const std::shared_ptr<spdlog::logger>& log,
        const std::vector<NetStation>& stations,
        const std::vector<std::map<std::string, double>>& elev_of,
        long long week, double tow, double pl_now_s, EpochEvents& events);

    /**
     * @brief The declared anchor handover's transform of one (satellite, signal) family
     *        through the traversal of carried references
     *        (@c CarryMechanism::DeclaredHandover).
     *
     * Every carried ambiguity of the family moves by @f$-\Delta@f$, the satellite's carried
     * phase bias of that signal by @f$+\Delta@f$, and the s_prev of every phase-bias coupling
     * term on it in the ambiguity carry by @f$+\Delta@f$ (variances untouched: an exact basis
     * change).  The sites the transform does not move are declared with their reason in the
     * mechanism table.
     *
     * With @p complete (@c network.carry_rebase_complete) the transform
     * also moves the sites the table exempts as owed, each by the same rule: the gross
     * screen's held ambiguity of the family by @f$-\Delta@f$ (as the ambiguity), the last
     * solved phase bias (@c pb_level_prev) and an open pin bridge of the family ("SAT_SIGNAL")
     * by @f$+\Delta@f$ (as the carried phase bias), and the s_prev of every phase-bias
     * coupling term on the family's column in every other coupling map (the wet delay, the
     * slant ionosphere and its block, the gross screen's three holds, the receiver clock,
     * phase bias, code bias and the phase colour) by @f$+\Delta@f$.  The phase hold's
     * held value is an emitted value and is not moved.
     * @param carry    the run's carried state.
     * @param entities the run's entity tables (the station carries).
     * @param sat      the family's satellite.
     * @param sig      the family's signal.
     * @param delta    the incoming anchor's carried level (m), read before anything moves.
     * @param n_shift  out: carried ambiguities moved (the held ambiguity moved under
     *                 @p complete is counted in @p counts only).
     * @param n_pb     out: carried phase biases moved (the last solved phase bias and the
     *                 bridge moved under @p complete are counted in @p counts only).
     * @param n_cpl    out: coupling terms moved (every coupling map's).
     * @param counts   optional out: per-site tallies (accumulated).
     * @param complete also move the owed sites (false = the traversal walk only).
     */
    void declaredHandoverShiftCarry(NetworkCarryState& carry, net::NetworkEntities& entities,
                                    const std::string& sat,
                                    const std::string& sig, double delta,
                                    long long& n_shift, long long& n_pb, long long& n_cpl,
                                    CarrySiteCounts* counts = nullptr, bool complete = false);

    /**
     * @brief Declared anchor S-transform, replacing the network-wide
     *        erase of @c networkApplyPhaseAnchorCarryReref for continuous handovers.
     *
     * Requires @c float_kf and @c network.amb_float_declared_handover.  Without it, a
     * (satellite, signal) family's re-reference drops every station's carry for that
     * family in one epoch, and a state erased at every anchor handover does not persist
     * as a state.  The frame does step, so
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
     * reported, never thrown (the @c selfcheck_ok pattern of
     * @ref networkHandoverStransform).
     *
     * Two defects this function must not introduce:
     *  (i) The carry S-transformation @c eq34ShiftCarry in @c ChainDatum.cpp
     *      shifts @c lc.value_abs and every coupling
     *      @c s_prev on a shifted PB column together, because a coupling row's
     *      RHS is @f$y=(v-x_{lin})+\sum_j M_j\hat s_{prev,j}@f$ and, for a
     *      gauge-invariant row under a known frame step, the two moves cancel exactly.
     *      Shifting one and not the other injects the frame step as row bias.  This
     *      function moves both, plus the shared PB carried prior, the third reference
     *      on the same identity.
     *  (ii) The transform is reported, never silent.  One
     *      @c STRANSFORM row per shifted family (@c sts_applied, or @c sts_selfcheck_fail
     *      when the self-check fails) carries the outgoing and incoming anchors, Δ, the
     *      number of carries shifted and the self-check residual.
     *
     * Scope.  Only a continuous handover has a Δ: the successor was elected with a
     * carried level, and that level is the new basis.  A cold re-realization has no
     * ledger entry for the successor, so its family keeps the erase; cold handovers,
     * an unknown incoming station, and a continuous handover whose incoming anchor
     * holds no live carry are counted separately as refused.  Δ = 0 is the identity:
     * the family is taken over without a shift.  How often a handover is cold depends on
     * the phase-datum election policy.
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

    struct NetworkRun;      // network/driver/Run.hpp
    struct NetworkEpoch;    // network/driver/Epoch.hpp

    /**
     * @brief The epoch form of the bounded drop: builds this epoch's LOS
     *        elevation map when the birth floor is armed, tallies the response latch per cause,
     * then runs networkAmbFloatApplyDrops.
     *
     * Serial, after PASS A's slip scans have latched relevel.free_ambs and before the parallel
     * PASS-B inject reads the carry.  A no-op unless the vehicle is float_kf.
     *
     * @param run The run: ctx (the carry, the counters, the trace buffer), the configuration,
     *            the logger, the stations.
     * @param ep  The epoch, read: se (week, tow), pass_a.modeled (elevations), clock.pl_now_s.
     * @param events The epoch's events; written: slip (relevel).
     */
    void networkAmbFloatDropEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events);
    // ---- Run-end reports ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the ambiguity vehicle and the joint-block PD-fallback bar.
     *
     * The sections: the vehicle line, the bounded drop, the drop decouple, the drop
     * floor, the declared S-transform, and the joint-block PD (positive definite)
     * fallback bar, which fails the run when no block was attempted or more than 1 % fell
     * back.
     *
     * Gate: each section under its own condition:
     * `ctx.knobs.amb_float_kf || config_.getNetworkConfig().trace_amb_state`;
     * `ctx.knobs.amb_float_kf && ctx.knobs.amb_birth_var_floor > 0.0`;
     * `ctx.knobs.amb_drop_decouple_always`; `ctx.knobs.amb_float_kf`;
     * `ctx.knobs.amb_float_kf && ctx.knobs.amb_float_declared_handover`;
     * `ctx.knobs.pl_blk_from_vehicle`.
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 1 when the joint-block PD-fallback bar fails (under the float vehicle's joint
     * block, `ctx.knobs.pl_blk_from_vehicle`: zero blocks attempted, or a fallback rate
     * above 1 %); 0 otherwise.
     */
    int networkAmbVehicleRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

/// @brief Run-scope counters of AmbFloat, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct AmbFloatCounters {
    long long n_amb_drop_epochs       = 0;  ///< epochs on which at least one drop happened.
    long long n_amb_drop_relevel_free = 0;  ///< …of which from the detectors' latched free-epoch set (the level vehicle skips one inject instead).
    long long n_amb_drop_stale_gap    = 0;  ///< …of which from the inject-site gap cliff (the level vehicle skips one inject instead).
    long long n_amb_drop_idempotent   = 0;  ///< drop calls that found the arc already at or below birth strength: two detectors on one arc in one epoch, or an arc that never converged.  The age clock still restarts; the variance does not move.
    long long n_amb_stransform        = 0;  ///< declared anchor S-transformations applied to the carry, one per (sat,signal) family per handover.
    long long n_amb_stransform_cols   = 0;  ///< …carried columns shifted by them.
    long long n_amb_sts_identity      = 0;  ///< …families whose Δ was exactly 0 (already in the new basis: a station re-pinning itself): the transform is the identity, and the family is still owned (no erase).
    long long n_amb_sts_refused_cold  = 0;  ///< handovers refused by the transform because the election was cold (no carried level for the successor, so no new basis to express the family in): the erase stands; this is the limit of the transform.
    long long n_amb_sts_refused_no_level = 0; ///< …refused because the ledger said continuous but the incoming anchor held no live carry this epoch: a ledger/carry disagreement, counted apart from a cold election.
    long long n_amb_sts_selfcheck_fail = 0; ///< transforms whose exactness self-check failed: the incoming anchor's carried level did not read 0 within kStsExactTolM afterwards.  A transform defect, never noise; logged as a warning with identities.
};

}  // namespace GnssProcessor
