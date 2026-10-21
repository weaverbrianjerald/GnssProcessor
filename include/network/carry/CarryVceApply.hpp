#pragma once
/**
 * @file CarryVceApply.hpp
 * @brief LASER's variance-factor iteration applied to the network carry-row
 *        stochastic model (@c network.carry_vce_apply).
 *
 * The monitor (@ref NetworkCarryVce) measures the carry residuals against the
 * hand carry floors, and on quiet epochs finds those floors optimistic by more than
 * an order of magnitude.  Varying the floor showed that the formal DD sigma then
 * follows the floor rather than the data (a 40× floor change moved the formal dd_σ
 * by 14 % with identical actual behaviour), so the formal covariance in the
 * products, the URA channel included, is set by the floor at the carry level.
 * This class feeds the monitor's estimate back into the fold variances by the
 * standard positivity-guaranteed method:
 *
 *   NGS LASER Guide v2.7 slides 5-93..5-95 (PDF pages 405 to 407): variance-component
 *   estimation "by use of factors" (Koch 1999 sections 3.6.1 to 3.6.2; Lucas 1985).  Each
 *   observation group @f$i@f$ carries a multiplicative variance factor
 *   @f$\alpha_i^2@f$; the factor is updated from the group's estimated variance
 *   of unit weight as
 *   @f[ (\alpha_i^2)_j \;=\; (\hat\sigma_i^2)_{j-1}\,(\alpha_i^2)_{j-1}, @f]
 *   and the iteration has converged when every @f$\hat\sigma_i^2 \approx 1@f$.
 *   The factor is a product of positive numbers, so no iteration can drive a
 *   variance negative.  BIQUUE is permanently out of scope (slide 5-96): it adds
 *   a failure mode (Cholesky termination on a transient negative component) and
 *   agreed with the factor method in every LASER test.
 *
 * ## Streaming form, per (family × constellation) group @c g
 *
 * ```
 *   target_g       = robust_var_r_g / hand_fold_var_r_g     both from the monitor
 *   step           = clamp( target_g / α²_g , [1/slew, slew] )
 *   α²_g          <- clamp( α²_g · step , [1, carry_vce_apply_max] )
 *   fold_var       = hand model evaluated as before, then × α²_g   at every fold site
 *   sigma_hat_post = target_g / α²_g                        -> 1 at convergence
 * ```
 *
 * @c sigma_hat_post is LASER's @f$\hat\sigma_i^2@f$, the variance of unit weight
 * of group g under the weights the solve used, and is emitted per group per epoch
 * as the slide prescribes (@c network_carry_vce_apply.csv).
 *
 * ## The redundancy belongs in the target
 *
 * LASER's group estimate divides the weighted residual square sum by the group's
 * redundancy, @f$\hat\sigma_i^2 = \hat e_i^T P_i \hat e_i / r_i@f$.  The carry-row
 * residual (the prior minus the solved value) has @f$E\{\hat e^2\} = r\,\sigma^2_{used}
 * \hat\sigma^2@f$ with @f$r = 1 - w v@f$, so the monitor's @c robust_var_r (the
 * geometric mean of @f$\hat e^2/r@f$) over @c hand_fold_var_r is
 * @f$\hat\sigma^2\alpha^2@f$, the next factor of the iteration.
 *
 * A target without the division, @c robust_floor / @c hand_fold_var, under-inflates.
 * For a scalar carry with true prior variance @f$a\,p@f$ (p the hand fold
 * variance) and data variance d, @f$E\{\hat e^2\} = r^2(a p + d)@f$, and that target's
 * fixed point solves @f$\alpha^4 p^2 + \alpha^2 p d + d^2 = \alpha^2 a p^2@f$: below
 * a always, and with no solution above 1 once d exceeds about a·p/4, where the factor
 * stays pinned at 1.  The under-inflation is largest exactly where the carry is
 * stiff against its data (small r), the case the factor exists for.
 *
 * ## Why the loop is stable
 *
 * The target depends on α² only through the redundancy, which the monitor reads at
 * the weights actually used.  In the scalar model the target is
 * @f$f(\alpha^2) = \alpha^2 (a p + d)/(\alpha^2 p + d)@f$: increasing, with its only
 * positive fixed point at @f$\alpha^2 = a@f$ and slope @f$d/(a p + d) < 1@f$ there.
 * The update is therefore a first-order tracker toward a level, read through the
 * monitor's window lag: with a positive contraction the lagged recursion
 * @f$\delta_{k+1} = (1-s)\delta_k + s\,f'\delta_{k-L}@f$ has coefficients summing below
 * one, so it decays without alternating at any lag L.
 *
 * This is what separates it from the form that failed.  A step that multiplies the
 * factor by @f$\hat\sigma^2@f$ every epoch is an integrator: closed through the
 * monitor's exponential window, whose mean lag is
 * @f$\varphi/(1-\varphi) \approx 86@f$ epochs at the default 60-epoch half-life, a
 * saturated 5 %/epoch ramp overshoots its target by
 * @f$e^{86\ln 1.05} \approx 66\times@f$ before the effect reaches the measurement,
 * then oscillates.  Here @c step is formed against the current α² toward a level
 * that moves less than α² does, so the same ramp approaches monotonically and stops.
 *
 * Rows without redundancy (r at or below the monitor's floor, or r < 0 where the
 * coupled ambiguity carry harvests a conditional variance above the prior) carry no
 * information on the factor and are left out of @c robust_var_r.  The warm-up gate
 * counts the rows that remain (@c n_eff_r), and the trace reports both counts and
 * the mean redundancy (@c rbar).  On the coupled @c pl family r is formed from the
 * harvested conditional variance, so it is an approximation there; read @c rbar and
 * @c n_eff_r against @c n_eff before trusting a @c pl factor.
 *
 * ## Guards (none is a tuning choice)
 *
 * - **Inflate-only**: @f$\alpha^2 \ge 1@f$ relative to the hand floor, enforced by
 *   the clamp.  Deflation is the dangerous direction: a prior stiffer than the
 *   truth locks in a stale value, while a looser prior only converges more slowly.  The clamp bounds
 *   α² below at 1 but does not make it monotone: the factor falls back toward 1
 *   as the window forgets an event era (the anti-ratchet property).
 * - **Slew limiter** (@c carry_vce_apply_slew, default 1.05 per epoch, both directions).
 *   A ramp at the configured rate limit can still discharge, so the slew
 *   is not the safety argument; the inflate-only clamp is.  The slew keeps the
 *   solve's conditioning smooth.
 * - **Freeze on a contaminated epoch** (the monitor's own @c clean gate) and below
 *   @c carry_vce_apply_min_rows effective rows per group.  A frozen group is flagged in the
 *   trace and counted.
 * - **Every clamp and freeze edge is an event with an identity**.
 *   This class changes how much information a carried prior
 *   injects, so it is a coupling mechanism and is instrumented at both edges.
 *
 * ## Out of scope
 *
 * No deflation below the hand floors.  No measurement-side R adaptation: adaptive
 * q on the measurement side was falsified, and this class acts on the
 * prediction side only.  No per-satellite factors: factors are pooled per
 * constellation.  No Rate component: the field is
 * always rate-frozen by constant-gap collinearity, so only the Floor is scaled.
 * No BIQUUE.
 *
 * ## Isolation / testing
 *
 * Pure numeric leaf class over @ref NetworkCarryVce, with no @c ApplicationNetwork
 * coupling and no state it does not own.  Tested directly in
 * @c tests/network/test_carry_vce_apply.cpp against a monitor fed synthetic rows of known
 * truth, following the standalone-class pattern of @c test_network_carry_vce.
 */

#include "common/GnssIdentity.hpp"   // letterOfSat
#include "network/carry/CarryVce.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief LASER 5-95 variance-factor iteration over the carry families.
 *
 * One instance per producer run, held in @c NetworkRunContext::carry (the α²
 * state persists across epochs).  A default-constructed instance is inert:
 * @ref enabled is false, @ref factor returns exactly 1.0, and no call site's
 * output changes.
 */
class NetworkCarryVceApply {
public:
    /// Carry family slots.  Fixed and small so @ref factor is an array read in
    /// the parallel produce phase rather than a keyed lookup per carry row.
    enum Family : int {
        kPl = 0,   ///< phase-leveling ambiguity carry (@c network.phase_leveling_var_floor_m2).
        kPb = 1,   ///< shared satellite phase-bias carry (@c shared_carry_var_floor_pb_m2).
        kCb = 2,   ///< shared satellite code-bias  carry (@c shared_carry_var_floor_cb_m2).
        kNumFamilies = 3
    };

    /// Why a group did not step this epoch (@c kNone, whose name is "", means it stepped).
    enum class Freeze : int {
        kNone = 0,
        kEpochDirty,   ///< the epoch was contaminated (datum handover / reref storm).
        kWarmup,       ///< the group is below @c carry_vce_apply_min_rows effective rows with redundancy (@c n_eff_r).
        kNoTarget      ///< the monitor could not form a positive, finite target.
    };

    /// Per-(family × constellation) factor state and its run tallies.
    struct Group {
        int         family = -1;      ///< @ref Family slot.
        char        sys    = '\0';    ///< RINEX constellation letter (G/E/C/R…); '\0' = pooled.
        double      alpha2 = 1.0;     ///< the live variance factor (≥ 1 by construction).
        double      target = 1.0;     ///< last formed robust_var_r / hand_fold_var_r.
        double      sigma_hat_post = 1.0; ///< LASER's σ̂² under the weights actually used = target/α².
        double      robust_floor = 0.0;   ///< monitor readback (m²), uncorrected for redundancy, for the trace.
        double      hand_fold_var = 0.0;  ///< monitor readback (m²), for the trace.
        double      robust_var_r = 0.0;   ///< monitor readback (m²): the target's numerator.
        double      hand_fold_var_r = 0.0;///< monitor readback (m²): the target's denominator.
        double      rbar   = 0.0;     ///< monitor's geometric-mean redundancy of the target's rows.
        double      n_eff  = 0.0;     ///< monitor's forgetting-weighted row count at the last step.
        double      n_eff_r = 0.0;    ///< the same count over the target's rows (the warm-up gate).
        Freeze      freeze = Freeze::kWarmup;  ///< this epoch's freeze state.
        bool        at_ceiling = false;        ///< α² is pinned at @c carry_vce_apply_max.
        bool        at_unit    = false;        ///< α² is pinned at the inflate-only floor of 1.
        // --- run tallies (an active path must be countable) ---
        long long   n_steps      = 0; ///< epochs the factor was allowed to move.
        long long   n_frozen     = 0; ///< epochs frozen (any reason).
        long long   n_slew_up    = 0; ///< steps clipped by the slew limiter, inflating.
        long long   n_slew_down  = 0; ///< steps clipped by the slew limiter, deflating.
        long long   n_ceiling_ep = 0; ///< epochs spent pinned at @c carry_vce_apply_max.
        long long   n_unit_ep    = 0; ///< epochs spent pinned at 1 (target below the hand floor).
        double      alpha2_max   = 1.0; ///< largest α² reached over the run.
    };

    /// One clamp / freeze edge (both edges are reported, with identity).
    struct Edge {
        int         family = -1;
        char        sys    = '\0';
        const char* what   = "";   ///< "ceiling" | "unit" | "freeze".
        bool        entered = false;
        double      alpha2 = 1.0;
        double      target = 1.0;
        const char* reason = "";   ///< freeze reason on a freeze edge; "" otherwise.
    };

    /// Inert until @ref configure: the factor table is unity, so a fold site that
    /// multiplies by @ref factor unconditionally is byte-identical to one that
    /// does not.
    NetworkCarryVceApply() { for (auto& row : alpha2_) row.fill(1.0); }

    /**
     * @brief Arm the apply path.
     * @param families   carry families to scale (subset of the monitor's; the
     *                   loader rejects anything else).  Empty ⇒ inert.
     * @param slew       per-epoch maximum factor step, × and ÷ (values ≤ 1 are
     *                   treated as "no limit", i.e. 1 step to target).
     * @param alpha2_max ceiling on α² (≥ 1; a smaller value is raised to 1).
     * @param min_rows   freeze a group below this many effective (forgetting-
     *                   weighted) monitor rows with redundancy.
     */
    void configure(const std::vector<std::string>& families,
                   double slew, double alpha2_max, long long min_rows) {
        wanted_.fill(false);
        any_ = false;
        for (const auto& f : families) {
            const int s = familySlot(f);
            if (s >= 0) { wanted_[static_cast<std::size_t>(s)] = true; any_ = true; }
        }
        slew_      = (slew > 1.0) ? slew : 0.0;          // 0 ⇒ unlimited
        alpha2_max_ = (alpha2_max >= 1.0) ? alpha2_max : 1.0;
        min_rows_  = (min_rows > 0) ? static_cast<double>(min_rows) : 0.0;
        for (auto& row : alpha2_) row.fill(1.0);
    }

    /// @return true when at least one family is armed (the apply path is live).
    bool enabled() const { return any_; }

    /// @return true when @p family (slot) is scaled by this instance.
    bool wants(int family_slot) const {
        return any_ && family_slot >= 0 && family_slot < kNumFamilies
            && wanted_[static_cast<std::size_t>(family_slot)];
    }

    /**
     * @brief The variance factor a fold site multiplies into its hand fold
     *        variance; exactly 1.0 when the path is off, the family is unarmed,
     *        or the group has no factor yet.
     *
     * O(1) array read with no allocation and no locking: safe to call from the
     * PASS-B parallel produce phase, where it is invoked once per carried
     * ambiguity per station.  The table is refreshed only by @ref stepEpoch,
     * which runs serially at the end of the epoch.
     *
     * @param family_slot @ref Family value.
     * @param sys         RINEX constellation letter of the carried key.
     */
    double factor(int family_slot, char sys) const {
        if (!wants(family_slot)) return 1.0;
        const unsigned c = static_cast<unsigned char>(sys);
        if (c >= kSysSlots) return 1.0;
        return alpha2_[static_cast<std::size_t>(family_slot)][c];
    }

    /// Convenience overload: takes the satellite id and uses its constellation letter.
    double factor(int family_slot, const std::string& sat) const {
        return sat.empty() ? 1.0 : factor(family_slot, letterOfSat(sat));
    }

    /**
     * @brief Advance every armed group's factor by one LASER 5-95 iteration.
     *
     * Call exactly once per epoch, after every feed into @p mon for that epoch
     * and before the next epoch's folds.  With that ordering the fold of epoch N
     * is scaled by α² estimated from every clean carry row through epoch N−1,
     * and no row can calibrate against its own fold; no other ordering gives
     * both.
     *
     * @param mon    the carry-row monitor (read-only).
     * @param clean  false on a contaminated epoch ⇒ every group freezes.  Mirrors
     *               @c NetworkCarryVce::beginEpoch's gate: a datum handover's
     *               re-levels are not carry noise, and a factor fed with them
     *               would inflate on a frame step.
     * @param edges  appended with every clamp/freeze edge crossed this epoch
     *               (both directions).  Never cleared by this call.
     */
    void stepEpoch(const NetworkCarryVce& mon, bool clean,
                   std::vector<Edge>* edges = nullptr) {
        if (!any_) return;
        ++n_epochs_;
        for (const auto& fc : mon.keys()) {
            const int fslot = familySlot(fc.first);
            if (!wants(fslot)) continue;
            const char sys = fc.second.empty() ? '\0' : fc.second[0];
            const unsigned c = static_cast<unsigned char>(sys);
            if (c >= kSysSlots) continue;
            Group& g = groups_[groupKey(fslot, sys)];
            g.family = fslot;
            g.sys    = sys;

            const NetworkCarryVce::Estimate e = mon.estimate(fc.first, fc.second);
            g.robust_floor    = e.robust_floor;
            g.hand_fold_var   = e.hand_fold_var;
            g.robust_var_r    = e.robust_var_r;
            g.hand_fold_var_r = e.hand_fold_var_r;
            g.rbar            = e.rbar;
            g.n_eff           = e.n_eff;
            g.n_eff_r         = e.n_eff_r;

            // The warm-up counts the rows the target is formed from (those with
            // redundancy), not every row the monitor saw.
            const Freeze was_frozen = g.freeze;
            Freeze freeze = Freeze::kNone;
            if (!clean)                       freeze = Freeze::kEpochDirty;
            else if (e.n_eff_r < min_rows_)   freeze = Freeze::kWarmup;
            else if (!(e.robust_var_r > 0.0) || !std::isfinite(e.robust_var_r)
                  || !(e.hand_fold_var_r > 0.0) || !std::isfinite(e.hand_fold_var_r))
                                              freeze = Freeze::kNoTarget;
            g.freeze = freeze;

            if (freeze != Freeze::kNone) {
                ++g.n_frozen;
                // The readout stays current while frozen: sigma_hat_post still
                // measures how far the folded model is from the data; the factor
                // just did not act this epoch.  A blank column would read as "no
                // data" when the data exist and were not applied.
                if (e.hand_fold_var_r > 0.0 && std::isfinite(e.hand_fold_var_r)
                    && std::isfinite(e.robust_var_r))
                    g.target = e.robust_var_r / e.hand_fold_var_r;
                g.sigma_hat_post = (g.alpha2 > 0.0) ? g.target / g.alpha2 : 0.0;
                if (edges && was_frozen == Freeze::kNone)
                    edges->push_back(Edge{fslot, sys, "freeze", true, g.alpha2,
                                          g.target, freezeName(freeze)});
                continue;
            }
            if (edges && was_frozen != Freeze::kNone)
                edges->push_back(Edge{fslot, sys, "freeze", false, g.alpha2,
                                      g.target, freezeName(was_frozen)});

            g.target = e.robust_var_r / e.hand_fold_var_r;

            // --- LASER 5-95: α² <- σ̂² · α², slewed, then clamped ---------------
            double step = g.target / g.alpha2;              // == σ̂² under the used weights
            if (slew_ > 1.0) {
                if (step > slew_)        { step = slew_;        ++g.n_slew_up;   }
                else if (step < 1.0 / slew_) { step = 1.0 / slew_; ++g.n_slew_down; }
            }
            double a2 = g.alpha2 * step;
            if (!std::isfinite(a2) || !(a2 > 0.0)) a2 = g.alpha2;

            // Inflate-only.  Deflation below the hand floor is the lock-in
            // direction; the clamp makes it unreachable.
            const bool was_unit    = g.at_unit;
            const bool was_ceiling = g.at_ceiling;
            if (a2 < 1.0)          a2 = 1.0;
            if (a2 > alpha2_max_)  a2 = alpha2_max_;
            g.at_unit    = (a2 <= 1.0);
            g.at_ceiling = (a2 >= alpha2_max_) && (alpha2_max_ > 1.0);

            g.alpha2 = a2;
            g.sigma_hat_post = g.target / g.alpha2;
            ++g.n_steps;
            if (g.at_ceiling) ++g.n_ceiling_ep;
            if (g.at_unit)    ++g.n_unit_ep;
            if (a2 > g.alpha2_max) g.alpha2_max = a2;

            if (edges) {
                if (g.at_ceiling != was_ceiling)
                    edges->push_back(Edge{fslot, sys, "ceiling", g.at_ceiling,
                                          a2, g.target, ""});
                if (g.at_unit != was_unit)
                    edges->push_back(Edge{fslot, sys, "unit", g.at_unit,
                                          a2, g.target, ""});
            }
            alpha2_[static_cast<std::size_t>(fslot)][c] = a2;
        }
    }

    /// Every group seen so far, in (family, constellation) order.
    const std::map<std::string, Group>& groups() const { return groups_; }

    /// Epochs @ref stepEpoch was called (the denominator for every tally).
    long long epochs() const { return n_epochs_; }

    double slew()      const { return slew_; }
    double alpha2Max() const { return alpha2_max_; }
    double minRows()   const { return min_rows_; }

    /// Family name ↔ slot.  @return -1 for an unknown name.
    static int familySlot(const std::string& f) {
        if (f == "pl") return kPl;
        if (f == "pb") return kPb;
        if (f == "cb") return kCb;
        return -1;
    }
    static const char* familyName(int slot) {
        switch (slot) {
            case kPl: return "pl";
            case kPb: return "pb";
            case kCb: return "cb";
            default:  return "?";
        }
    }
    static const char* freezeName(Freeze f) {
        switch (f) {
            case Freeze::kEpochDirty: return "epoch_dirty";
            case Freeze::kWarmup:     return "warmup";
            case Freeze::kNoTarget:   return "no_target";
            default:                  return "";
        }
    }

private:
    static constexpr unsigned kSysSlots = 128;   ///< ASCII-indexed constellation table.

    static std::string groupKey(int fslot, char sys) {
        std::string k = familyName(fslot);
        k += '|';
        if (sys) k += sys;
        return k;
    }

    std::array<bool, kNumFamilies> wanted_{{false, false, false}};
    bool   any_        = false;
    double slew_       = 0.0;      ///< 0 ⇒ unlimited.
    double alpha2_max_ = 1.0;
    double min_rows_   = 0.0;
    long long n_epochs_ = 0;
    /// Hot lookup table read by the fold sites; [family][constellation letter].
    std::array<std::array<double, kSysSlots>, kNumFamilies> alpha2_{};
    std::map<std::string, Group> groups_;
};

struct NetworkRun;

/**
 * @brief Configures the carry-VCE apply step when it is armed.
 *
 * The monitor itself is an engine the run owns (run.engines.carry_vce).  Setup
 * builder: called once from Application::runNetwork.
 *
 * @param run The network run; fills run.ctx.carry.vce_apply.
 */
void networkCarryVceApplyArm(NetworkRun& run);

}  // namespace GnssProcessor
