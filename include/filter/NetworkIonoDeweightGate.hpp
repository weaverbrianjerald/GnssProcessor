#pragma once
/**
 * @file NetworkIonoDeweightGate.hpp
 * @brief Station-adaptive SH-VTEC-constraint deweight gate (PPP-RTK network producer).
 *
 * A self-contained, header-only per-station gate that produces a scalar deweight
 * factor @f$w_s \in (0,1]@f$ for each station's global-SH-VTEC ionosphere
 * constraint.  The producer multiplies the INFORMATION of a station's SH↔slant
 * iono pseudo-observations by @f$w_s@f$ (equivalently @f$\sigma^2 \times\!= 1/w_s@f$),
 * which — because those rows are a single JOINT pseudo-obs coupling the station's
 * per-LOS slant iono to the shared field in both directions — uniformly relaxes
 * (a) the station's LOS feeding the field fit and (b) the field constraining the
 * station's per-LOS iono.  @f$w_s\to0@f$ decouples the station's ionosphere
 * entirely (it floats on its own GF code/phase, still observable — no rank hole)
 * while the station keeps serving the shared clocks / code / phase biases / datum
 * graph / position.  It is the surgical alternative to dropping a hostile station.
 *
 * ## Why a network-RELATIVE robust gate (not an absolute σ threshold)
 *
 * The single-layer SH surface structurally cannot represent per-LOS slant
 * structure: ~70–90 % of LOS residuals exceed 3σ at ANY degree, at every network
 * scale.  That exceedance is a chronic, roughly common-mode background — an
 * ABSOLUTE 3σ gate would engage every station.  The genuinely hostile sites
 * (equatorial-anomaly / scintillation-belt stations at solar max, whose TRUE
 * slant iono is beyond the field's representability) stand out only as a RELATIVE
 * excess.  So the gate standardizes each station's smoothed misfit statistic
 * against the network's own robust center: @f$z_s = (\bar m_s - \mathrm{med})/
 * (1.4826\,\mathrm{MAD})@f$ over the mature, non-statically-listed stations.  The
 * median-centering removes the common-mode background; the MAD scale tolerates the
 * poison minority (breakdown point 50 %).
 *
 * ## Dynamics (IGG-style, hysteretic, rate-limited)
 *
 * Per station the raw per-epoch statistic @f$m_s@f$ (a standardized-residual RMS,
 * supplied by the caller) is EWMA-smoothed with a half-life window
 * (@f$\phi = 0.5^{1/W}@f$, the NetworkCarryVce forgetting idiom).  A station
 * ENGAGES when @f$z_s > \sigma_{hi}@f$ and RELEASES when @f$z_s < \sigma_{lo}@f$
 * (hysteresis band prevents chatter).  While engaged, the target weight follows a
 * smooth three-segment ramp — @f$w=1@f$ at @f$z=\sigma_{lo}@f$, @f$w=@f$ @c floor
 * at/above @f$z=\sigma_{hi}@f$, quadratic between — and the actual weight slews
 * toward it by at most @c rate_limit per clean epoch (anti-oscillation).  Datum /
 * pivot-handover epochs are excluded (their residual spikes are datum artifacts,
 * not station misfit), mirroring the carry-VCE clean gate.  Nothing here is ever
 * NaN: every divide is guarded and every output is clamped to @f$[floor, 1]@f$.
 *
 * ## Two paths, one gate
 *
 *  - **v1 (static):** stations in the configured list are held at a fixed
 *    @c static_w (0 ⇒ free-floating iono).  They are excluded from the robust
 *    center so a known-poison site cannot mask the detection of others.
 *  - **v2 (adaptive):** the caller feeds every present station's statistic via
 *    @ref observe each clean epoch and calls @ref advance; @ref weight then
 *    returns the auto-tuned factor.
 *
 * Empty static list + adaptive off ⇒ @ref weight is 1.0 for every station ⇒ the
 * producer is byte-identical to OFF (invariant #6).
 *
 * ## Threading
 *
 * @ref weight is @c const and only reads the per-station map (find, never insert),
 * so it is safe to call from the parallel per-station assembly.  @ref observe /
 * @ref advance MUTATE and must be called only from the serial post-solve harvest —
 * exactly the sh_dev_carry / carry-VCE contract (aged/updated serially, read in
 * parallel).
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Per-station SH-VTEC-constraint deweight factor with a v1 static list and a
 *        v2 network-relative IGG auto-trigger.  Header-only, self-contained.
 */
class NetworkIonoDeweightGate {
public:
    NetworkIonoDeweightGate() = default;

    /**
     * @param static_stations v1: station long-names held at @p static_w.
     * @param static_w        v1 deweight factor for the listed stations (clamped to [0,1];
     *                        0 = free-floating iono / full decouple).
     * @param adaptive        v2: enable the network-relative auto-trigger.
     * @param window_epochs   v2 EWMA half-life (epochs) of the per-station statistic.
     * @param sigma_hi        v2 engage threshold on the robust z-score.
     * @param sigma_lo        v2 release threshold (hysteresis; forced below @p sigma_hi).
     * @param floor           v2 minimum weight at/above @p sigma_hi (clamped to [0,1)).
     * @param rate_limit      v2 max |Δw| per clean epoch (<=0 ⇒ snap, no slew limit).
     * @param min_epochs      v2 min clean EWMA samples before a station may gate.
     */
    NetworkIonoDeweightGate(std::set<std::string> static_stations, double static_w,
                            bool adaptive, double window_epochs, double sigma_hi,
                            double sigma_lo, double floor, double rate_limit, int min_epochs)
        : static_(std::move(static_stations)),
          static_w_(clamp01(static_w)),
          adaptive_(adaptive),
          phi_(window_epochs > 0.0 ? std::pow(0.5, 1.0 / window_epochs) : 0.0),
          sigma_hi_(sigma_hi),
          sigma_lo_(sigma_lo < sigma_hi ? sigma_lo : 0.5 * sigma_hi),
          floor_(std::min(0.999, std::max(0.0, floor))),
          rate_(rate_limit > 0.0 ? rate_limit : 1.0),
          min_epochs_(min_epochs > 0 ? min_epochs : 1) {
        if (static_w_ < 1.0)
            for (const auto& s : static_) ever_.insert(s);  // static stations are deweighted for the whole run
    }

    /// True if either path can deweight some station (else the gate is inert / OFF).
    bool active() const { return adaptive_ || (static_w_ < 1.0 && !static_.empty()); }
    /// True if the v2 adaptive auto-trigger is enabled.
    bool adaptive() const { return adaptive_; }

    /// v2: fold one present station's raw per-epoch misfit statistic (standardized-residual
    /// RMS).  Call for every present station before @ref advance.  No-op unless adaptive.
    void observe(const std::string& station, double stat) {
        if (!adaptive_ || !std::isfinite(stat)) return;
        pending_[station] = stat;
    }

    /// SPEC_day129_frame_guards P1-2: one engage/release transition of the LAST
    /// @ref advance call.  The run-end summary reports only TOTALS (326 engage /
    /// 307 release over the day), which cannot say whether a wave landed at the
    /// collapse epoch — that needs the per-epoch list.
    struct Transition {
        std::string station;
        double      z{0.0};      ///< robust z-score that tripped it
        double      w{1.0};      ///< weight AFTER this epoch's rate-limited slew
        bool        engaged{false};  ///< true = ENGAGE, false = RELEASE
    };

    /// v2: advance the network-relative robust gate one epoch.  @p clean == false (a datum /
    /// pivot handover this epoch) ⇒ discard this epoch's observations and HOLD all weights.
    void advance(bool clean) {
        last_transitions_.clear();   // P1-2: per-CALL, so a held epoch reports none
        last_released_.clear();      // release hygiene: same per-CALL contract
        if (!adaptive_) return;
        if (!clean) { pending_.clear(); return; }
        ++advances_;                 // counts only advances that actually ran
        // (1) EWMA update per observed station.
        for (const auto& kv : pending_) {
            Gate& g = gates_[kv.first];
            g.ewma = (g.n == 0) ? kv.second : phi_ * g.ewma + (1.0 - phi_) * kv.second;
            ++g.n;
        }
        pending_.clear();
        // (2) network robust center (median) + scale (1.4826*MAD) over mature, non-static gates.
        std::vector<double> vals;
        vals.reserve(gates_.size());
        for (const auto& kv : gates_)
            if (kv.second.n >= min_epochs_ && !isStatic(kv.first))
                vals.push_back(kv.second.ewma);
        double med = 0.0, mad_sigma = 0.0;
        if (vals.size() >= kMinPop) {
            med = median(vals);
            std::vector<double> dev;
            dev.reserve(vals.size());
            for (double a : vals) dev.push_back(std::fabs(a - med));
            mad_sigma = 1.4826 * median(dev);
        }
        // (3) per-station engage/release + rate-limited smooth ramp.
        std::size_t engaged = 0;
        for (auto& kv : gates_) {
            Gate& g = kv.second;
            if (isStatic(kv.first)) { g.w = 1.0; continue; }  // static path owns these
            double target = 1.0;
            if (g.n >= min_epochs_ && mad_sigma > 0.0) {
                g.z = (g.ewma - med) / mad_sigma;
                if (!g.engaged && g.z > sigma_hi_) {
                    g.engaged = true; ++trigger_;
                    last_transitions_.push_back({kv.first, g.z, g.w, true});   // P1-2
                } else if (g.engaged && g.z < sigma_lo_) {
                    g.engaged = false; ++release_;
                    last_transitions_.push_back({kv.first, g.z, g.w, false});  // P1-2
                }
                if (g.engaged) target = rampTarget(g.z);
            }
            const double w_prev = g.w;   // pre-slew; the release edge is w_prev -> g.w
            const double d = target - g.w;
            g.w += std::min(rate_, std::max(-rate_, d));
            g.w = std::min(1.0, std::max(floor_, g.w));
            // Release edge: the zero-weight era ENDS here.  Captured around the slew
            // rather than off the transition list — see enableReleaseTracking.
            if (release_track_ && w_prev <= release_eps_ && g.w > release_eps_)
                last_released_.push_back(kv.first);
            // P1-2: report the POST-slew weight (what the next epoch's assembly
            // will actually multiply the SH rows by), not the pre-slew value.
            if (!last_transitions_.empty() && last_transitions_.back().station == kv.first)
                last_transitions_.back().w = g.w;
            if (g.engaged) { ++engaged; ever_.insert(kv.first); }
        }
        const std::size_t static_cnt = (static_w_ < 1.0) ? static_.size() : 0;
        peak_ = std::max(peak_, static_cnt + engaged);
    }

    /// Deweight factor @f$w_s \in [floor,1]@f$ for a station (1.0 if unknown / not
    /// deweighted).  @c const and read-only — safe from the parallel assembly.
    double weight(const std::string& station) const {
        if (static_w_ < 1.0 && static_.count(station)) return static_w_;
        if (adaptive_) {
            const auto it = gates_.find(station);
            if (it != gates_.end()) return it->second.w;
        }
        return 1.0;
    }

    /// Smooth IGG three-segment ramp target for an engaged station's z-score:
    /// 1 at/below @c sigma_lo, @c floor at/above @c sigma_hi, quadratic between.
    double rampTarget(double z) const {
        const double zc = std::min(sigma_hi_, std::max(sigma_lo_, z));
        const double span = sigma_hi_ - sigma_lo_;
        const double t = (span > 0.0) ? (zc - sigma_lo_) / span : 1.0;  // 0 at release, 1 at trigger
        const double heal = 1.0 - t;
        return floor_ + (1.0 - floor_) * heal * heal;
    }

    // ---- Telemetry / inspection ----
    long long triggerEvents() const { return trigger_; }
    long long releaseEvents() const { return release_; }
    std::size_t peakDeweighted() const { return peak_; }
    const std::set<std::string>& everDeweighted() const { return ever_; }
    const std::set<std::string>& staticStations() const { return static_; }
    std::size_t staticCount() const { return (static_w_ < 1.0) ? static_.size() : 0; }
    double staticWeight() const { return static_w_; }
    bool engaged(const std::string& s) const {
        const auto it = gates_.find(s);
        return it != gates_.end() && it->second.engaged;
    }
    double zscore(const std::string& s) const {
        const auto it = gates_.find(s);
        return it != gates_.end() ? it->second.z : 0.0;
    }
    int samples(const std::string& s) const {
        const auto it = gates_.find(s);
        return it != gates_.end() ? it->second.n : 0;
    }

    /// SPEC_day129_frame_guards P1-2: engage/release transitions of the LAST
    /// @ref advance call (empty on a held / non-adaptive epoch).
    const std::vector<Transition>& lastTransitions() const { return last_transitions_; }

    // ---- SPEC_deweight_release_hygiene: the RELEASE EDGE -------------------
    // The engage/release TRANSITION list above cannot serve the release-hygiene
    // response, for two independent reasons:
    //   (a) a station's weight can rise off the floor with NO flag flip — an
    //       engaged station whose z decays toward sigma_lo gets rampTarget(z) > floor
    //       (and exactly 1.0 at z == sigma_lo) while `engaged` is still true, and a
    //       station that stops qualifying (n < min_epochs, or mad_sigma == 0) skips
    //       the whole engage/release block with target 1.0 — both slew UP silently;
    //   (b) Transition::w is PATCHED to the post-slew value, so a consumer cannot
    //       recover the pre-slew weight and therefore cannot tell a rise off zero
    //       from any other slew.
    // Hence the edge is captured where it physically happens — around the slew
    // itself — as "w was <= eps and is now > eps", i.e. this station's SH-tie rows
    // were NOT folded last epoch and WILL be folded next epoch.  Opt-in: an
    // unarmed run never allocates the list.

    /// Arm per-epoch release-edge capture with the zero-weight threshold @p eps
    /// (<= 0 falls back to @ref kZeroW, the same constant @ref aggregate uses for
    /// its n_zero count).  Idempotent; call once at construction time.
    void enableReleaseTracking(double eps) {
        release_track_ = true;
        release_eps_   = (eps > 0.0) ? eps : kZeroW;
    }
    /// Stations whose weight ROSE above the tracking eps in the LAST @ref advance
    /// call — i.e. whose zero-weight era ended.  Empty unless
    /// @ref enableReleaseTracking was called.  Refilled per-CALL, exactly like
    /// @ref lastTransitions.
    const std::vector<std::string>& lastReleased() const { return last_released_; }
    /// Number of @ref advance calls that actually ran (adaptive AND clean).
    ///
    /// A consumer OUTSIDE the caller's `if (sh_want_gate)` block MUST compare this
    /// against its own last-seen value before reading @ref lastReleased: on a
    /// datum-change epoch the producer skips advance() entirely (it does not call
    /// `advance(false)` — that branch has no caller in the tree), so both lists
    /// retain the PREVIOUS epoch's contents and an unguarded consumer double-fires.
    long long advanceCount() const { return advances_; }

    /// Zero-weight threshold: at or below this a station's SH block is dropped
    /// entirely (@ref weight <= 0 makes the assembly append no rows at all).
    static constexpr double kZeroW = 1e-9;

    /// P1-2: cheap per-epoch aggregate of the CURRENT weight field.
    struct Aggregate {
        int    n_engaged{0};  ///< adaptive gates currently engaged (excludes the static list)
        int    n_zero{0};     ///< stations at w <= kZeroW — SH block fully dropped, iono floating
        double min_w{1.0};    ///< smallest weight over all known stations (static included)
        double mean_w{1.0};   ///< mean weight over all known stations (static included)
    };
    /// Weight-field summary.  O(stations); safe to call once per epoch from the
    /// SERIAL harvest site (it only reads, but keep it on the same thread as
    /// @ref advance for consistency with the class's threading contract).
    Aggregate aggregate() const {
        Aggregate a;
        double sum = 0.0; std::size_t n = 0;
        for (const auto& kv : gates_) {
            const double w = isStatic(kv.first) ? static_w_ : kv.second.w;
            sum += w; ++n;
            a.min_w = std::min(a.min_w, w);
            if (w <= kZeroW) ++a.n_zero;
            if (kv.second.engaged && !isStatic(kv.first)) ++a.n_engaged;
        }
        // Static-listed stations with no adaptive gate entry still carry static_w_.
        for (const auto& s : static_) {
            if (gates_.count(s)) continue;
            sum += static_w_; ++n;
            a.min_w = std::min(a.min_w, static_w_);
            if (static_w_ <= kZeroW) ++a.n_zero;
        }
        a.mean_w = n ? sum / static_cast<double>(n) : 1.0;
        return a;
    }

private:
    struct Gate {
        double ewma{0.0};   ///< EWMA of the per-station misfit statistic.
        double w{1.0};      ///< Current deweight factor (slew-limited).
        double z{0.0};      ///< Last network-relative robust z-score.
        int    n{0};        ///< Clean EWMA sample count.
        bool   engaged{false};
    };

    static double clamp01(double x) { return x <= 0.0 ? 0.0 : (x >= 1.0 ? 1.0 : x); }
    bool isStatic(const std::string& s) const { return static_w_ < 1.0 && static_.count(s) > 0; }
    /// Upper-median via nth_element (deterministic; mutates @p v).
    static double median(std::vector<double>& v) {
        const std::size_t mid = v.size() / 2;
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
        return v[mid];
    }
    static constexpr std::size_t kMinPop = 8;  ///< Min mature stations to define an outlier.

    std::set<std::string> static_;
    double static_w_{0.0};
    bool   adaptive_{false};
    double phi_{0.0};
    double sigma_hi_{3.0};
    double sigma_lo_{2.0};
    double floor_{0.0};
    double rate_{0.25};
    int    min_epochs_{30};
    std::map<std::string, Gate>   gates_;
    std::map<std::string, double> pending_;
    long long   trigger_{0};
    long long   release_{0};
    std::size_t peak_{0};
    std::set<std::string> ever_;
    std::vector<Transition> last_transitions_;   ///< P1-2: refilled per advance()
    // --- SPEC_deweight_release_hygiene release-edge capture (opt-in) ---
    bool   release_track_{false};                ///< enableReleaseTracking() called.
    double release_eps_{kZeroW};                 ///< "decoupled" threshold for the edge.
    std::vector<std::string> last_released_;     ///< refilled per advance(); empty unless tracking.
    long long advances_{0};                      ///< advance() calls that ran (adaptive && clean).
};

}  // namespace GnssProcessor
