/**
 * @file NetworkAmbFloat.hpp
 * @brief RF-1 step 4 — the FLOORED BIRTH PRIOR and the BOUNDED DROP that replaces the
 *        leveling vehicle's four un-instrumented reset paths.
 *
 * Card: docs/refound/RF-1_float_ambiguities_design.md §3.3 / §3.4 / §7.2, pre-registered
 * in docs/refound/RF-1_steps12_read_2026-09-15.md.  Live only under
 * @c network.ambiguity_vehicle = "float_kf".
 *
 * WHAT IS WRONG TODAY (design §1.5, measured in steps 1–2).  Four paths return a carried
 * ambiguity to a level re-determined from ONE epoch, and none of them restores a
 * birth-scale variance: the free-epoch latch and the gap cliff SKIP one inject and let
 * the harvest re-stash at a phase-stiff variance; the T4 / T3 refloats and the gap prune
 * DELETE the entry.  The field trace priced the middle one: a @c t4_attrib erase throws
 * away a prior at 1.5e-6 m² and the successor arc starts at 0.114 m² — 76 000× looser —
 * and takes 31 folds to earn it back, 120 times an hour.
 *
 * THE REPLACEMENT, in one operator (§3.4).  A slip is a statement about CONFIDENCE, not
 * about IDENTITY, so a drop restores the birth prior and keeps everything else:
 *
 *     lc.var       = var_birth = max(var_first, floor(elevation))
 *     lc.coupling.clear();  lc.blk = nullptr;  lc.blk_idx = -1
 *     lc.relevel_s = now;   lc.dropped_ep = now_ep
 *     // value_abs, var_first, var_birth, birth_s, birth_ep, last_seen_s, n_folds KEPT
 *
 * and the prior IS STILL FOLDED this epoch, at birth strength.  Three consequences the
 * card argues and this build must deliver (standing invariant #7):
 *  1. **the decoupling edge is BOUNDED** — today a free epoch removes ALL information
 *     about the column for one epoch, opening an unbounded direction that the shared
 *     parameters re-balance into (R10's 88 % of unsignalled satellite-clock steps sit on
 *     a T4 refloat of the same satellite).  A drop removes information down to a STATED
 *     level, so the response is proportional;
 *  2. **the re-coupling edge re-validates by construction** — the arc re-earns its
 *     variance as information accumulates, and @c network_amb_state.csv's @c fold_var
 *     trajectory can watch it happen;
 *  3. **@c value_abs survives** — the manager's decision of 2026-09-14, question 2.  It
 *     is the linearization point, an exact solve no-op, and keeping it is what the
 *     lane-preserving refloat needs later.
 *
 * WHAT STAYS AN ERASE.  The gap PRUNE past @c phase_leveling_max_gap_s and an explicit
 * re-admission beyond the readmit horizon — the one case that means "this is a different
 * arc" (§3.4).  Both remain events with both edges counted.
 */
#pragma once

#include "filter/NetworkRunTypes.hpp"

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

    /// Cause tags for a drop — the @c event vocabulary of @c network_amb_state.csv and
    /// the key of the run-end inventory.  One string constant per site so the reader and
    /// the writer cannot drift.
    namespace AmbDropCause {
        inline constexpr const char* kReLevelFree = "relevel_free";  ///< a detector's latched free-epoch set (the five NetworkSlipQc sites).
        inline constexpr const char* kStaleGap    = "stale_gap";     ///< the inject-site gap cliff (gap > phase_leveling_max_gap_s, arc still present).
        inline constexpr const char* kT4Attrib    = "t4_attrib";     ///< RAIM prominence-attribution refloat (was an erase).
        inline constexpr const char* kT3Consensus = "t3_consensus";  ///< RAIM per-satellite consensus refloat (was an erase).
    }

    /**
     * @brief The BIRTH-PRIOR FLOOR at one line of sight (m²) — §3.3(b), elevation-mapped.
     *
     * The floor is a statement about what a CODE-determined level is worth, so it is
     * mapped exactly as the code row weights are: @f$\sigma^2(e)=\sigma^2_\text{zen}/
     * \sin^{2p}(e)@f$ with @p elev_power the posture's @c filter.code_elevation_power.
     * A flat zenith floor would be optimistic at exactly the low elevations that own the
     * metre-plus tail (§24.13 finding 2: the 25.8 % of iono rows below the tie cutoff).
     *
     * @param base_m2    @c network.amb_float_birth_var_m2 — the floor AT ZENITH (m²).
     *                   0 disables the floor entirely (returns 0).
     * @param elev_deg   LOS elevation (degrees).  A non-finite or non-positive value —
     *                   the "elevation unavailable" case — returns the ZENITH value,
     *                   which is the conservative direction (a smaller floor never
     *                   invents confidence the arc has not earned).
     * @param elev_power The σ-power of the code weights (@c getCodeElevationPower()).
     * @return The floor in m² at that elevation, clamped at the zenith value from below.
     */
    double ambFloatBirthFloor(double base_m2, double elev_deg, double elev_power);

    /**
     * @brief Apply the DROP operator to one carried entry, in place (§3.4).
     *
     * @param lc        The entry.  @c value_abs, @c var_first, @c var_birth, both birth
     *                  stamps, @c last_seen_s and @c n_folds are untouched.
     * @param floor_m2  This LOS's birth floor from @ref ambFloatBirthFloor; the restored
     *                  variance is @c max(var_birth, var_first, floor_m2) so a drop can
     *                  only ever LOOSEN the prior, never tighten one.
     * @param now_s     Continuous-GPS seconds — the new @c relevel_s (the age clock
     *                  restarts, exactly as it does on a re-level today).
     * @param now_ep    This epoch's @c n_loop_epochs — the new @c dropped_ep.
     * @return True when the entry actually changed (its variance was tighter than the
     *         birth prior).  False means the arc was already at or below birth strength,
     *         which is an idempotent re-drop within one epoch and is counted separately.
     */
    bool ambFloatDrop(LocalCarry& lc, double floor_m2, double now_s, long long now_ep);

    /**
     * @brief The SERIAL pre-assembly drop pass — the free-epoch latch and the gap cliff.
     *
     * Runs after PASS A's slip scans (which fill @c ctx.carry.relevel.free_ambs) and
     * BEFORE the PASS-B assembly, so by the time the parallel inject reads an entry the
     * drop has already happened and the inject simply folds it at birth strength instead
     * of skipping it.  Serial by construction: it mutates @c ctx.carry.local_carry, which
     * the produce phase then reads.
     *
     * Two causes, both of which SKIPPED an inject on the leveling vehicle:
     *  - @c relevel_free — every @c (station, amb key) the detectors latched this epoch.
     *    The latched set itself is untouched, because the lane observatory, the
     *    phase-datum candidacy screen, the anchor-slip handover staging and the harvest's
     *    statistics guards all read it (§4.1: "keep the latched-set plumbing");
     *  - @c stale_gap — every carried arc whose sighting gap already exceeds
     *    @c phase_leveling_max_gap_s.  The post-harvest PRUNE still erases the ones that
     *    are genuinely gone; this catches the ones that are PRESENT this epoch, which the
     *    prune structurally cannot see (the harvest refreshes @c last_seen_s first).
     *
     * A no-op unless the vehicle is @c float_kf.
     *
     * @param ctx       The run context (the carry, the knobs, the counters, the trace buffer).
     * @param config_   The run config (the birth floor and the code elevation power).
     * @param log       The network logger.
     * @param stations  The station roster — for the trace rows' station names.
     * @param elev_of   Per station, this epoch's LOS elevation by satellite (degrees);
     *                  empty when the floor is unarmed, in which case the zenith value is
     *                  used and the pass costs one map walk.
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
     * @brief RF-1 step 5 — the DECLARED ANCHOR S-TRANSFORM (design §3.6), replacing
     *        @c networkApplyPhaseAnchorCarryReref's network-wide erase.
     *
     * A state that is ERASED at every anchor handover is not a state (the manager's
     * decision of 2026-09-14, question 3).  Today a (satellite, signal) family's
     * re-reference drops EVERY station's carry for that family — a whole-family refloat,
     * every station, one epoch.  The frame did step, so the carried values ARE stale in
     * the old basis; but a frame step is a KNOWN, exact basis change, so the right answer
     * is to express the carry in the new basis rather than to throw it away:
     *
     *     Δ = value_abs(r_in, sat, signal)          // the incoming anchor's carried level
     *     for every station r carrying (sat, signal):  value_abs_r −= Δ
     *     the shared phase bias of (sat, signal):      value      += Δ
     *     every coupling s_prev on that PB column:                += Δ
     *     // VARIANCES UNTOUCHED — an exact basis change, not a refloat
     *
     * and the incoming anchor's own column then reads 0, which is exactly what its
     * @f$w=10^{10}@f$ gauge pin is about to assert.  That identity is checked to
     * @c kStsExactTolM and reported, never thrown — the @c sts_selfcheck_ok pattern of
     * @ref networkHandoverStransform.
     *
     * ⚠ TWO FILED LATENT DEFECTS THIS MUST NOT RE-OPEN.
     *  (i) @c NetworkChainDatum.cpp:852-866 — the D4 operator shifts @c lc.value_abs AND
     *      every coupling @c s_prev on a shifted PB column TOGETHER, because a Card-1
     *      coupling row's RHS is @f$y=(v-x_{lin})+\sum_j M_j\hat s_{prev,j}@f$ and a
     *      gauge-invariant row's correct y-change under a known frame step is the exact
     *      CANCELLATION of the two moves.  Shifting one and not the other injects the
     *      frame step as row bias.  This function does both, plus the shared PB carried
     *      prior, which is the third reference on the same identity;
     *  (ii) invariant #7 — it is an EVENT with both edges: a @c STRANSFORM /
     *      @c sts_applied row per family carries the outgoing and incoming anchors, Δ,
     *      the number of carries shifted and the self-check residual.
     *
     * SCOPE.  Only a CONTINUOUS handover has a Δ: the successor was elected with a
     * carried level, and that level is the new basis.  A COLD re-realization has no
     * ledger entry for the successor — there is nothing to transform to — so its family
     * keeps the ERASE, and the two populations are counted separately.  That split is
     * the honest limit of this step, and it is why RF-2 owns the ELECTION policy that
     * makes cold handovers rare.
     *
     * @param ctx         The run context (the carry, the shared prior stash, the counters).
     * @param log         The network logger.
     * @param traces      The trace streams — the @c STRANSFORM event rows.
     * @param stations    The station roster (resolving the incoming anchor's index).
     * @param handovers   THIS epoch's anchor handovers, from the election.
     * @param week        GPS week of the epoch.
     * @param tow         GPS time of week (s) of the epoch.
     * @param handled_out Receives every (sat, signal) family this transform took
     *                    ownership of; the caller REMOVES them from the erase set, so a
     *                    family is transformed or erased, never both.
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
