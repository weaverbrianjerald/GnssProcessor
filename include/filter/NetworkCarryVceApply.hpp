#pragma once
/**
 * @file NetworkCarryVceApply.hpp
 * @brief M2 v2 (@c network.carry_vce_apply) — LASER's variance-factor iteration
 *        applied to the network carry-row stochastic model.
 *
 * The M2 monitor (@ref NetworkCarryVce) measured the hand carry floors 30–120×
 * optimistic on quiet epochs, and the M3 floor ladder proved the consequence: a
 * 40× floor swing moved the formal dd_σ by 14 % with IDENTICAL actual behaviour,
 * i.e. the formal covariance the products carry (URA channel included) is a
 * FLOOR ARTIFACT at the carry level.  This class closes the loop the monitor
 * left open, by the standard positivity-guaranteed route:
 *
 *   NGS LASER Guide v2.7 slides 5-93..5-95 (PDF pp. 405–407) — variance-component
 *   estimation "by use of factors" (Koch 1999 §§3.6.1–3.6.2; Lucas 1985).  Each
 *   observation group @f$i@f$ carries a multiplicative variance factor
 *   @f$\alpha_i^2@f$; the factor is updated from the group's estimated variance
 *   of unit weight as
 *   @f[ (\alpha_i^2)_j \;=\; (\hat\sigma_i^2)_{j-1}\,(\alpha_i^2)_{j-1}, @f]
 *   and the iteration has converged when every @f$\hat\sigma_i^2 \approx 1@f$.
 *   Positive by construction — the factor is a product of positive numbers, so no
 *   iteration can drive a variance negative.  Slide 5-96 is why BIQUUE is out of
 *   scope PERMANENTLY: it adds an iteration-death mode (Cholesky termination on a
 *   transient negative component) and agreed with the factor method in every LASER
 *   test.
 *
 * ## Streaming form, per (family × constellation) group @c g
 *
 * ```
 *   target_g       = robust_floor_g / hand_fold_var_g       both from the M2 monitor
 *   step           = clamp( target_g / α²_g , [1/slew, slew] )
 *   α²_g          <- clamp( α²_g · step , [1, apply_max] )
 *   fold_var       = hand model evaluated as today, then × α²_g   at every fold site
 *   sigma_hat_post = target_g / α²_g                        -> 1 at convergence
 * ```
 *
 * @c sigma_hat_post IS LASER's @f$\hat\sigma_i^2@f$ — the variance of unit weight
 * of group g under the weights the solve ACTUALLY used — so the readout the slide
 * prescribes is emitted per group per epoch (@c network_carry_vce_apply.csv).
 *
 * ## Why the target is lag-free (and why that is not a detail)
 *
 * Both halves of @c target are α²-INDEPENDENT: @c robust_floor is the χ²₁-corrected
 * geometric mean of ê² alone, and @c hand_fold_var is the geometric mean of the
 * fold variance as the HAND model formed it (the monitor is fed both, see
 * @c NetworkCarryVce::addRow).  So the update is a first-order tracker of a fixed
 * point, not a closed feedback loop through a lagged measurement.
 *
 * That distinction is load-bearing.  Had the step been driven by
 * @f$\hat\sigma^2@f$ measured from the standardized residuals — the naive reading
 * of 5-95 — the loop would close through the monitor's exponential window, whose
 * MEAN LAG is @f$\varphi/(1-\varphi) \approx 86@f$ epochs at the default 60-epoch
 * half-life.  A saturated 5 %/epoch ramp against an 86-epoch lag overshoots its
 * target by @f$e^{86\ln 1.05} \approx 67\times@f$ before it can see itself, then
 * rings.  With an α²-independent target the same ramp approaches monotonically and
 * STOPS, because @c step is formed against the CURRENT α².
 *
 * ## Guards (each is a standing invariant, not a tuning choice)
 *
 * - **Inflate-only, structurally**: @f$\alpha^2 \ge 1@f$ relative to the hand
 *   floor.  The dangerous direction is DEFLATION — a stiffer-than-truth prior is
 *   the lock-in mechanism of record (standing invariant #1; the DOY-129 pivot-CB
 *   ratchet, q_cb = 1e-10 ⇒ a 49 m step with τ = 3.79 h).  Looser-than-truth is
 *   safe-but-slower.  The clamp kills that whole failure class by construction
 *   rather than by tuning.  Note this bounds α² BELOW at 1, it does not make α²
 *   monotone: the factor descends freely back toward 1 as the window forgets an
 *   event era (the anti-ratchet, and the iono-2 v3 demean precedent).
 * - **Slew limiter** (@c apply_slew, default 1.05 per epoch, both directions).
 *   Era-2's lesson is that a ramp AT the configured rate limit still discharges,
 *   so the ramp is explicitly NOT the safety argument here — the inflate-only
 *   clamp is.  The slew exists to keep the solve's conditioning quiet.
 * - **Freeze on a contaminated epoch** (the monitor's own @c clean gate) and below
 *   @c apply_min_rows effective rows per group.  A frozen group is FLAGGED in the
 *   trace and counted, never silent (census convention).
 * - **Every clamp and freeze edge is an EVENT with an identity** — standing
 *   invariant #7: this class is itself a coupling mechanism (it changes how much
 *   information a carried prior injects), so it is instrumented as one at BOTH
 *   edges.
 *
 * ## What it deliberately does NOT do
 *
 * No deflation below the hand floors; no measurement-side R adaptation (the era-2
 * ground — adaptive-q on the MEASUREMENT side was falsified, the PREDICTION side
 * is where this lives and where it stays); no per-satellite factors (constellation
 * pooling only — the D5 population precedent); no Rate component (the field is
 * ALWAYS rate-frozen by constant-gap collinearity, so Floor-only); no BIQUUE.
 *
 * ## Isolation / testing
 *
 * Pure numeric leaf class over @ref NetworkCarryVce — no @c ApplicationNetwork
 * coupling, no state it does not own.  Driven directly from
 * @c tests/test_carry_vce_apply.cpp against a monitor fed synthetic rows of known
 * truth, the standalone-class precedent of @c test_network_carry_vce.
 */

#include "filter/NetworkCarryVce.hpp"

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
 * @brief LASER 5-95 variance-factor iteration over the carry families (M2 v2).
 *
 * One instance per producer run, living in @c NetworkRunContext::carry (the α²
 * state is cross-epoch by construction).  Default-constructed it is INERT:
 * @ref enabled is false, @ref factor returns exactly 1.0, and no call site
 * changes a byte.
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

    /// Why a group did not step this epoch (@c "" ⇒ it did).
    enum class Freeze : int {
        kNone = 0,
        kEpochDirty,   ///< the epoch was contaminated (datum handover / reref storm).
        kWarmup,       ///< the group is below @c apply_min_rows effective rows.
        kNoTarget      ///< the monitor could not form a positive, finite target.
    };

    /// Per-(family × constellation) factor state and its run tallies.
    struct Group {
        int         family = -1;      ///< @ref Family slot.
        char        sys    = '\0';    ///< RINEX constellation letter (G/E/C/R…); '\0' = pooled.
        double      alpha2 = 1.0;     ///< the live variance factor (≥ 1 by construction).
        double      target = 1.0;     ///< last formed robust_floor / hand_fold_var.
        double      sigma_hat_post = 1.0; ///< LASER's σ̂² under the weights actually used = target/α².
        double      robust_floor = 0.0;   ///< monitor readback (m²), for the trace.
        double      hand_fold_var = 0.0;  ///< monitor readback (m²), for the trace.
        double      n_eff  = 0.0;     ///< monitor's forgetting-weighted row count at the last step.
        Freeze      freeze = Freeze::kWarmup;  ///< this epoch's freeze state.
        bool        at_ceiling = false;        ///< α² is pinned at @c apply_max.
        bool        at_unit    = false;        ///< α² is pinned at the inflate-only floor of 1.
        // --- run tallies (standing invariant #4: a live wire must be countable) ---
        long long   n_steps      = 0; ///< epochs the factor was allowed to move.
        long long   n_frozen     = 0; ///< epochs frozen (any reason).
        long long   n_slew_up    = 0; ///< steps clipped by the slew limiter, inflating.
        long long   n_slew_down  = 0; ///< steps clipped by the slew limiter, deflating.
        long long   n_ceiling_ep = 0; ///< epochs spent pinned at @c apply_max.
        long long   n_unit_ep    = 0; ///< epochs spent pinned at 1 (target below the hand floor).
        double      alpha2_max   = 1.0; ///< largest α² reached over the run.
    };

    /// One clamp / freeze EDGE (standing invariant #7 — both edges, with identity).
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
     * @param min_rows   freeze a group below this many EFFECTIVE (forgetting-
     *                   weighted) monitor rows.
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
     * @brief The variance factor a fold site must multiply into its hand fold
     *        variance — exactly 1.0 when the path is off, the family is unarmed,
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

    /// Convenience overload: takes the satellite id and uses its first character.
    double factor(int family_slot, const std::string& sat) const {
        return sat.empty() ? 1.0 : factor(family_slot, sat[0]);
    }

    /**
     * @brief Advance every armed group's factor by ONE LASER 5-95 iteration.
     *
     * MUST be called exactly once per epoch, AFTER every feed into @p mon for
     * that epoch and BEFORE the next epoch's folds — that ordering is what makes
     * the contract "the fold of epoch N is scaled by α² estimated from every
     * clean carry row through epoch N−1" true, and it is the only ordering under
     * which no row can calibrate against its own fold.
     *
     * @param mon    the M2 monitor (read-only).
     * @param clean  false on a contaminated epoch ⇒ every group FREEZES.  Mirrors
     *               @c NetworkCarryVce::beginEpoch's gate, because a datum
     *               handover's re-levels are not carry noise and a factor that
     *               ate them would be inflating on a frame step.
     * @param edges  appended with every clamp/freeze EDGE crossed this epoch
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
            g.robust_floor  = e.robust_floor;
            g.hand_fold_var = e.hand_fold_var;
            g.n_eff         = e.n_eff;

            const Freeze was_frozen = g.freeze;
            Freeze freeze = Freeze::kNone;
            if (!clean)                       freeze = Freeze::kEpochDirty;
            else if (e.n_eff < min_rows_)     freeze = Freeze::kWarmup;
            else if (!(e.robust_floor > 0.0) || !std::isfinite(e.robust_floor)
                  || !(e.hand_fold_var > 0.0) || !std::isfinite(e.hand_fold_var))
                                              freeze = Freeze::kNoTarget;
            g.freeze = freeze;

            if (freeze != Freeze::kNone) {
                ++g.n_frozen;
                // The readout stays LIVE while frozen: sigma_hat_post is still the
                // honest "how far off is the model we are folding at", it just did
                // not act this epoch.  A blank column here would read as "no data"
                // when the truth is "data, deliberately not applied".
                if (e.hand_fold_var > 0.0 && std::isfinite(e.hand_fold_var)
                    && std::isfinite(e.robust_floor))
                    g.target = e.robust_floor / e.hand_fold_var;
                g.sigma_hat_post = (g.alpha2 > 0.0) ? g.target / g.alpha2 : 0.0;
                if (edges && was_frozen == Freeze::kNone)
                    edges->push_back(Edge{fslot, sys, "freeze", true, g.alpha2,
                                          g.target, freezeName(freeze)});
                continue;
            }
            if (edges && was_frozen != Freeze::kNone)
                edges->push_back(Edge{fslot, sys, "freeze", false, g.alpha2,
                                      g.target, freezeName(was_frozen)});

            g.target = e.robust_floor / e.hand_fold_var;

            // --- LASER 5-95: α² <- σ̂² · α², slewed, then clamped ---------------
            double step = g.target / g.alpha2;              // == σ̂² under the used weights
            if (slew_ > 1.0) {
                if (step > slew_)        { step = slew_;        ++g.n_slew_up;   }
                else if (step < 1.0 / slew_) { step = 1.0 / slew_; ++g.n_slew_down; }
            }
            double a2 = g.alpha2 * step;
            if (!std::isfinite(a2) || !(a2 > 0.0)) a2 = g.alpha2;

            // INFLATE-ONLY.  Deflation below the hand floor is the lock-in
            // direction (standing invariant #1) and is structurally unreachable.
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

}  // namespace GnssProcessor
