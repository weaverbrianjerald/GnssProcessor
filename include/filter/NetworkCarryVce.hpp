#pragma once
/**
 * @file NetworkCarryVce.hpp
 * @brief Least-squares variance-component monitor of the network carry-row
 *        stochastic model (M2, LS-VCE carry-q).
 *
 * A self-contained, header-only sequential accumulator that estimates the
 * variance components of the phase-leveling / shared-carry pseudo-observation
 * rows from their post-fit innovations, so that the hand-set carry floors
 * (@c network.phase_leveling_var_floor_m2, @c network.shared_carry_var_floor_*_m2)
 * and process-noise rates (@c network.phase_leveling_q_amb, @c network.q_pb/q_cb)
 * can be checked against a measured σ̂.  The class does not write into the solve:
 * run as a monitor alone, it leaves the network solution byte-identical.  Two
 * consumers read its estimates and change the solve when armed: the v2
 * variance-factor path (@c network.carry_vce_apply, NetworkCarryVceApply.hpp) and
 * the D5 v2 innovation gate (@c network.carry_invariant_innov_calibrate, in
 * NetworkInvariantCarryDriver).  Foundation: Teunissen & Amiri-Simkooei 2008
 * (J Geod 82:65–82, LS-VCE), their Corollary 4 (residual form) and §3.2
 * (sequential estimation).
 *
 * ## What the monitor answers
 *
 * Each epoch a carried ambiguity (or shared bias) is folded as a pseudo-
 * observation of the state with prior mean @c b̄ (the carried value) and prior
 * variance @c fold_var = max(var_prev + q·Δt, floor).  After the solve the state
 * is re-estimated as @c x̂ with posterior variance @c v.  The carry-row
 * innovation
 * @f[ \hat e = \bar b - \hat x @f]
 * does not depend on how the fold is represented: a scalar diagonal prior and the
 * Card-1 coupling rows both encode the same marginal prior
 * @f$N(\bar b,\,\text{fold\_var})@f$ on that one state.  A single hook at the
 * post-solve carry harvest therefore captures it for every carry wire format.
 *
 * ## The estimator (residual-form LS-VCE, fixed weight)
 *
 * The carry-row covariance is modelled as a linear combination of known
 * cofactors, @f$ \sigma^2_{yy,i} = \sum_k \sigma_k\, c^{(k)}_i @f$, with two
 * components per carry family:
 *   - **Floor**: cofactor @f$c^{(0)}_i = 1@f$, unit m² (the measured counterpart
 *     of the hand @c var_floor; physically the un-modelled leveling error).
 *   - **Rate**: cofactor @f$c^{(1)}_i = \Delta t_i@f$ (the arc gap, s), unit
 *     m²/s (the measured counterpart of the hand @c q_amb; ≈0 if the carry is a
 *     random constant).
 *
 * The weight is the config weight the solve used, @f$w_i = 1/\text{fold\_var}_i@f$,
 * and the partial redundancy of the carry row is
 * @f[ r_i \;=\; 1 - w_i\,v_i \;\in[0,1], @f]
 * which is exact for that row because @c v is the actual posterior variance of the
 * state.  The redundancy is taken block-diagonal in the carry rows: the
 * cross-redundancy between distinct carry rows is neglected (the approximation
 * documented in the M2 spec).  With @f$\hat e_i@f$ the observed innovation, the
 * per-row LS-VCE normal matrix and right-hand side accumulate as
 * @f[ N_{kl} \mathrel{+}= w_i^2\, c^{(k)}_i\, c^{(l)}_i\, r_i, \qquad
 *     l_k    \mathrel{+}= w_i^2\, c^{(k)}_i\, \hat e_i^2. @f]
 * @f$N_{kl}@f$ carries a single factor @f$r_i@f$, not @f$r_i^2@f$: that is the
 * block-diagonal expectation of @f$\mathrm{tr}(Q_k W P^\perp Q_l W P^\perp)@f$, and
 * it makes @f$E[l_k]=\sum_l N_{kl}\sigma_l@f$ hold.  The component estimate is
 * @f$\hat\sigma = N^{-1} l@f$ with dispersion @f$D(\hat\sigma) = 2N^{-1}@f$ (the
 * Gaussian LS-VCE precision).  Because the weight @f$w_i@f$ is held at the config
 * value (not iterated toward @f$\hat\sigma@f$), @f$\hat\sigma@f$ is an unbiased
 * one-step calibration of the config model: it answers "are the config floors
 * consistent with the residuals?" directly, with no re-solve.  Minimum variance
 * would need @f$w=\sigma^{-1}@f$, which is the v2 concern; the fixed-weight
 * estimator is still unbiased for any positive @f$w@f$.
 *
 * A weak Gaussian a-priori on @f$\sigma@f$ (worth about one row) regularises the
 * normal matrix so that it stays invertible when the Floor and Rate cofactors are
 * near-collinear.  They are collinear when every arc gap equals the decimation
 * interval (Δt constant, so the two columns differ only by that constant).  The
 * condition number is reported, so a Rate held at its a-priori is visible in the
 * output.
 *
 * ## Clean-epoch gating
 *
 * The caller decides which rows are clean.  Contaminated carries (cycle slip or
 * refloat, datum re-reference, pivot or datum handover, RAIM exclusion) must be
 * withheld, or their re-level inflates @f$\hat e^2@f$ and biases @f$\hat\sigma@f$
 * upward.  @ref beginEpoch takes a @c clean flag (false: every @ref addRow call of
 * that epoch is ignored), and the caller calls @ref addRow only for carries that
 * are clean per row.  @ref beginEpoch also advances the exponential forgetting, so
 * the estimate tracks a trailing window of emitted epochs.
 *
 * ## Isolation and testing
 *
 * Pure numeric leaf class: no @c ApplicationNetwork coupling and no parallel state
 * ownership (it does not touch @c StateManager).  It takes carry-row innovations,
 * variances and gaps, and returns σ̂ per component per (family, constellation).
 * It is driven directly from @c tests/test_network_carry_vce.cpp with synthetic
 * residuals of known truth, following the standalone-class pattern of
 * @c NeqAccumulator and @c CombinedCorrectionSigma.
 */

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @brief LS-VCE monitor of network carry-row variance components (M2, v1).
 *
 * One instance per producer run.  Keyed internally by (family, constellation),
 * where @c family is a carry kind requested by the config (@c "pl", @c "pb",
 * @c "cb") and @c constellation is the single RINEX system letter (G/E/C/R…).
 * Splitting by constellation costs nothing and is more informative than a global
 * pool; a thin constellation whose rows never outweigh the a-priori reports
 * approximately the seed.
 */
class NetworkCarryVce {
public:
    /// Variance-component index within a (family, constellation) accumulator.
    enum Component : int {
        kFloor = 0,   ///< cofactor 1  → σ̂ in m²   (measured counterpart of @c var_floor).
        kRate  = 1,   ///< cofactor Δt → σ̂ in m²/s (measured counterpart of @c q_amb).
        kNumComponents = 2
    };

    /// Result of solving one (family, constellation) accumulator.
    struct Estimate {
        double        robust_floor = 0.0; ///< Primary output: geometric-mean (χ²₁-corrected)
                                          ///  innovation variance (m²), a tail-robust estimate of
                                          ///  r̄·(true carry var).  Dominated by the typical carry,
                                          ///  not by the rare frame-churn re-levels that dominate
                                          ///  the arithmetic mean.
        double        hand_fold_var = 0.0;///< Geometric mean of the hand-model fold variance (m²)
                                          ///  over the same forgetting window, i.e.
                                          ///  `max(var_prev + q·Δt, var_floor)` before any variance
                                          ///  factor is applied.  It is the denominator of the target
                                          ///  of the LASER 5-95 factor iteration, and it does not
                                          ///  depend on α², so the apply loop has no feedback lag
                                          ///  (see NetworkCarryVceApply.hpp §"why the target is
                                          ///  lag-free").  Falls back to @c seedFloor() with no rows.
                                          ///  (M2 v2, docs/SPEC_carry_vce_apply.md)
        std::array<double, kNumComponents> sigma{{0.0, 0.0}};  ///< arithmetic-mean LS-VCE σ̂ (Floor m², Rate m²/s); includes event rows.
        std::array<double, kNumComponents> std{{0.0, 0.0}};    ///< formal 1σ of the mean σ̂ (√ of 2·N⁻¹ diag).
        double        cond      = 0.0;    ///< condition number of the data normal matrix (Rate separability).
        double        n_eff     = 0.0;    ///< forgetting-weighted effective clean-row count.
        long long     n_rows    = 0;      ///< total clean rows accumulated over the run (undecayed).
        bool          rate_frozen = false;///< N ill-conditioned: Rate is held at its a-priori (report only).
        long long     floor_clamped = 0;  ///< 1 when the Floor positivity clamp bound in this solve, else 0.
        long long     rate_clamped  = 0;  ///< 1 when the Rate positivity clamp bound in this solve, else 0.
    };

    /**
     * @brief Construct the monitor.
     * @param families         carry kinds to accumulate (e.g. {"pl","pb","cb"}).
     * @param window_epochs    exponential-forgetting half-life in emitted epochs
     *                         (≤0: no forgetting; the whole run accumulates).
     * @param split_by_const   key per constellation letter (true) or pool all
     *                         satellites into one "" bucket (false).
     * @param seed_floor_m2    a-priori Floor σ (m²), the config @c var_floor; the
     *                         weak prior mean, which the data quickly outweigh.
     *                         ≤0 selects kSeedFloorFallback (1e-6 m²).
     * @param seed_rate_m2ps   a-priori Rate σ (m²/s), the config @c q_amb
     *                         (0 for a random-constant carry; <0 is taken as 0).
     * @param typ_gap_s        nominal arc gap (s; ≤0 is taken as 60).  It sets the
     *                         lower bound seed_floor/typ_gap on the Rate prior
     *                         scale, which is the scale used when @p seed_rate_m2ps
     *                         is 0.
     * @param prior_rows       a-priori strength in equivalent clean rows (small,
     *                         default 1, enough to keep N invertible; ≤0 is taken
     *                         as 1).
     */
    NetworkCarryVce(std::vector<std::string> families,
                    double window_epochs,
                    bool   split_by_const   = true,
                    double seed_floor_m2    = 0.0,
                    double seed_rate_m2ps   = 0.0,
                    double typ_gap_s        = 60.0,
                    double prior_rows       = 1.0)
        : families_(std::move(families)),
          split_by_const_(split_by_const),
          seed_floor_(seed_floor_m2 > 0.0 ? seed_floor_m2 : kSeedFloorFallback),
          seed_rate_(seed_rate_m2ps >= 0.0 ? seed_rate_m2ps : 0.0),
          prior_rows_(prior_rows > 0.0 ? prior_rows : 1.0) {
        // A natural Rate scale for the prior: a rate that, over one nominal gap,
        // fills the seed floor.  Keeps the prior's Rate information finite even
        // when the config rate (and hence the prior mean) is exactly 0.
        const double gap = (typ_gap_s > 0.0) ? typ_gap_s : 60.0;
        rate_scale_ = std::max(seed_rate_, seed_floor_ / gap);
        if (!(rate_scale_ > 0.0)) rate_scale_ = seed_floor_ / gap;
        // Prior information (Fisher, in the σ metric): worth ~prior_rows rows.
        i0_floor_ = prior_rows_ / (seed_floor_ * seed_floor_);
        i0_rate_  = prior_rows_ / (rate_scale_ * rate_scale_);
        phi_ = (window_epochs > 0.0) ? std::pow(0.5, 1.0 / window_epochs) : 1.0;
    }

    /// @return true when at least one family is requested (monitor is live).
    bool enabled() const { return !families_.empty(); }

    /**
     * @brief Seed one (family, sat) accumulator from a snapshot, so that a warm
     *        resume uses the restored inflation immediately (RTB-7(d), RE-1).
     *
     * Without this a resumed run restarts the accumulator empty, so every consumer
     * gated on a warm-up row count (e.g. @c kInflationWarmupRows of the Card-4 D5
     * gate) falls back to its raw-formal test for a whole window, which is the
     * regime D5 v2 exists to correct, while the restored carry presents a full set
     * of chords to gate.  Constructing with @c seed_floor_m2 does not achieve this:
     * the seed floor is only the weak prior mean, and @ref estimate reports
     * @c n_eff = 0, so the warm-up gate still rejects the estimate.
     *
     * Seeds the tail-robust geometric-mean channel exactly: @ref estimate inverts
     * @c geo/kChi2GeoMean, so writing @c sum_log_e2 = n_eff·log(floor·kChi2GeoMean)
     * reproduces @p robust_floor_m2 and @p n_eff exactly.  The arithmetic-mean
     * LS-VCE channel (N, l → @c Estimate::sigma / @c cond) is left prior-only on
     * purpose: it is a reporting instrument, and the gate reads only
     * @c robust_floor and @c n_eff.  Forgetting then decays the seeded mass exactly
     * as if it had been accumulated.
     *
     * @param family          accumulator family (must be one of the constructed families).
     * @param sat             satellite id (bucketed per @c split_by_const).
     * @param robust_floor_m2 restored @c Estimate::robust_floor (>0).
     * @param n_eff           restored forgetting-weighted row count (>0).
     * @param n_rows          restored undecayed row total (report only).
     */
    void seedRobustFloor(const std::string& family, const std::string& sat,
                         double robust_floor_m2, double n_eff, long long n_rows) {
        if (!wants(family) || !(robust_floor_m2 > 0.0) || !(n_eff > 0.0)) return;
        Accum& a = accum_[key(family, sat)];
        a.n_eff      = n_eff;
        a.n_rows     = n_rows;
        a.sum_log_e2 = n_eff * std::log(robust_floor_m2 * kChi2GeoMean);
        // The snapshot carries no fold-variance history, so the seeded mass is
        // credited to the a-priori floor (M2 v2).  Estimate::hand_fold_var is then
        // defined (never 0/0) on a resumed instance without assuming what those
        // rows were folded at.  For the same reason the v2 apply path is
        // fresh-convergence-only (SPEC_carry_vce_apply.md §8, its one field-blocking
        // deferral).  The D5 consumer of this seed reads only robust_floor and n_eff.
        a.sum_log_fv = n_eff * std::log(seed_floor_);
    }

    /**
     * @brief Advance to a new emitted epoch: apply forgetting and set the
     *        epoch-level clean gate.
     *
     * Forgetting is applied on every call, whether or not the epoch is clean.
     *
     * @param content_time_s epoch content time (continuous GPS s).  Stored in
     *                       @c last_content_s_; no member of this class reads it.
     * @param clean          false marks a contaminated epoch (datum handover,
     *                       re-reference storm) whose rows are all ignored.
     */
    void beginEpoch(double content_time_s, bool clean) {
        epoch_clean_    = clean;
        last_content_s_ = content_time_s;
        if (phi_ < 1.0) {
            for (auto& kv : accum_) kv.second.decay(phi_);
        }
    }

    /**
     * @brief Fold one clean carry-row innovation into its accumulator.
     * @param family        carry kind ("pl"/"pb"/"cb"); ignored if not requested.
     * @param sat           satellite id (constellation = first char).
     * @param innovation_m  ê = carried_prior − solved (m).
     * @param gap_s         arc gap Δt since the carry was last seen (s).
     * @param post_var_m2   posterior variance v of the state after the solve (m²).
     * @param fold_var_m2   the prior variance the fold actually used (m²) = 1/w.
     * @param hand_fold_var_m2  M2 v2: the same variance as the hand model forms it,
     *                      i.e. before @c network.carry_vce_apply multiplies in the
     *                      variance factor α².  Pass ≤0 (the default) when no
     *                      factor is applied, and the fold value is used; call
     *                      sites without a factor therefore accumulate the same
     *                      @c Estimate::hand_fold_var as before v2.
     *
     * No-op unless the monitor is enabled, the epoch is clean, @p family is
     * requested, @p fold_var_m2 is positive, @p gap_s is non-negative and the
     * inputs are finite.  The innovation feeds both the tail-robust geometric-mean
     * accumulator (which uses only ê²) and the arithmetic-mean LS-VCE normals
     * (which also use the redundancy r = 1 − w·v, clamped to [ε,1] with
     * ε = kRedundancyFloor = 1e-3, because under the coupling carry the harvested
     * @p post_var_m2 is a conditional variance that can exceed the prior).
     */
    void addRow(const std::string& family, const std::string& sat,
                double innovation_m, double gap_s,
                double post_var_m2, double fold_var_m2,
                double hand_fold_var_m2 = -1.0) {
        if (!epoch_clean_ || !wants(family)) return;
        if (!(fold_var_m2 > 0.0) || !std::isfinite(fold_var_m2)) return;
        if (!std::isfinite(innovation_m) || !std::isfinite(post_var_m2)) return;
        if (!(gap_s >= 0.0) || !std::isfinite(gap_s)) return;
        const double e2  = innovation_m * innovation_m;
        Accum& a = accum_[key(family, sat)];
        // Tail-robust accumulator: Σ log(ê²).  The geometric mean is insensitive to
        // the rare frame-churn re-levels that dominate the arithmetic mean: a single
        // 100 m² outlier adds only log(100) to a sum over 10⁵ rows.  It measures the
        // typical carry disagreement, which is the floor being calibrated.
        a.sum_log_e2 += std::log(e2 > kTinyE2 ? e2 : kTinyE2);
        // The hand fold variance, accumulated on the same rows and the same
        // forgetting window, so that both quantities in the factor iteration's
        // target ê²/fv are measured over one population (M2 v2).  It is a geometric
        // mean for the same reason as the numerator: the fold variance inherits the
        // innovation's re-level tail through var_prev.
        const double fv_hand = (hand_fold_var_m2 > 0.0 && std::isfinite(hand_fold_var_m2))
                             ? hand_fold_var_m2 : fold_var_m2;
        a.sum_log_fv += std::log(fv_hand);
        a.n_eff += 1.0;
        ++a.n_rows;
        // Arithmetic-mean LS-VCE normals (include event rows; secondary output).
        // The redundancy is clamped rather than the row dropped, so the mean is
        // always defined.
        const double w = 1.0 / fold_var_m2;                 // config weight the solve used
        double r = 1.0 - w * post_var_m2;                   // partial redundancy (block-diagonal)
        if (r < kRedundancyFloor) r = kRedundancyFloor;
        if (r > 1.0) r = 1.0;
        const double w2r = w * w * r;
        const double c1  = gap_s;                           // Rate cofactor; Floor cofactor is 1
        // Unbiased diagonal LS-VCE normal matrix N_kl = Σ_i w_i² c^k_i c^l_i r_i
        // (a single partial redundancy r_i: the block-diagonal expectation of
        // tr(Q_k W P⊥ Q_l W P⊥) that makes E[l_k]=Σ_l N_kl σ_l hold; c0=1, c1=Δt).
        a.n00 += w2r;
        a.n01 += c1 * w2r;
        a.n11 += c1 * c1 * w2r;
        // l_k = Σ_i w_i² c^k_i ê_i²  (Q_k W ê W ê, diagonal W Q_k W = diag(w² c^k)).
        a.l0  += w * w * e2;
        a.l1  += w * w * c1 * e2;
    }

    /**
     * @brief Solve the (family, constellation) accumulator for σ̂ and its formal std.
     *
     * Adds the weak a-priori, inverts the 2×2 normal matrix, clamps the
     * components to be positive (Floor) / non-negative (Rate) and reports the
     * condition number and clamp flags.  When the data condition number is at or
     * above kCondMax (1e6), solves for the Floor alone with the Rate held at its
     * a-priori.  Never throws.
     */
    Estimate estimate(const std::string& family, const std::string& sat) const {
        Estimate out;
        const auto it = accum_.find(key(family, sat));
        const Accum a = (it != accum_.end()) ? it->second : Accum{};
        out.n_eff  = a.n_eff;
        out.n_rows = a.n_rows;

        // --- Primary output: tail-robust geometric-mean floor ---------------
        // geo_mean(ê²) = exp(Σ log ê² / n).  For ê ~ N(0,s), ê²/s ~ χ²₁ whose log
        // has mean E[log χ²₁] = ψ(½)+ln2 = −γ−ln2 = −1.27036 (not ψ(½) = −1.96351),
        // so geo_mean(ê²) = s·e^(−1.27036) = s·0.280730 = s·kChi2GeoMean.  Invert to
        // recover s = the typical innovation variance (= r̄·true carry var).
        if (a.n_eff > 0.0) {
            const double geo = std::exp(a.sum_log_e2 / a.n_eff);   // geometric mean of ê²
            out.robust_floor = geo / kChi2GeoMean;                 // ÷ e^(−1.27036)
            out.hand_fold_var = std::exp(a.sum_log_fv / a.n_eff);  // target denominator (M2 v2)
        } else {
            out.robust_floor  = seed_floor_;
            out.hand_fold_var = seed_floor_;
        }

        // --- Secondary output: arithmetic-mean LS-VCE (includes event rows) --
        // Separability is a property of the data, not of the regularised matrix:
        // when every arc gap equals the decimation interval the Floor/Rate
        // cofactors are collinear and the data normal matrix is rank-deficient
        // (cond_data → ∞), whereas a spread of gaps gives cond_data ~ 1e2–1e4.
        const double d_tr   = a.n00 + a.n11;
        const double d_det  = a.n00 * a.n11 - a.n01 * a.n01;
        const double d_disc = std::sqrt(std::max(0.0, d_tr * d_tr - 4.0 * d_det));
        const double d_lmax = 0.5 * (d_tr + d_disc);
        const double d_lmin = 0.5 * (d_tr - d_disc);
        out.cond = (d_lmin > 0.0) ? d_lmax / d_lmin
                                  : std::numeric_limits<double>::infinity();
        out.rate_frozen = !(out.cond < kCondMax);

        if (out.rate_frozen) {
            // Rate unidentifiable (on field data: near-constant gaps).  Solve for
            // the Floor alone, the well-conditioned direction, with the Rate held
            // at its a-priori.  This keeps the field floor stable: in the joint 2×2
            // solve an unconstrained Rate can drive the Floor negative.
            const double A00 = a.n00 + i0_floor_;
            double s0 = (A00 > 0.0) ? (a.l0 + i0_floor_ * seed_floor_ - a.n01 * seed_rate_) / A00
                                    : seed_floor_;
            if (!(s0 > kSigmaFloor)) { s0 = kSigmaFloor; ++out.floor_clamped; }
            out.sigma = {s0, seed_rate_};
            out.std   = {(A00 > 0.0) ? std::sqrt(2.0 / A00) : 0.0, rate_scale_};
            return out;
        }

        // Rate identifiable: the full 2-component prior-regularised solve.
        const double A00 = a.n00 + i0_floor_;
        const double A11 = a.n11 + i0_rate_;
        const double A01 = a.n01;
        const double b0  = a.l0 + i0_floor_ * seed_floor_;
        const double b1  = a.l1 + i0_rate_  * seed_rate_;
        const double det = A00 * A11 - A01 * A01;
        if (!(det > 0.0) || !std::isfinite(det)) {
            out.sigma = {seed_floor_, seed_rate_};
            out.std   = {seed_floor_, rate_scale_};
            return out;
        }
        const double inv00 =  A11 / det;
        const double inv11 =  A00 / det;
        const double inv01 = -A01 / det;
        double s0 = inv00 * b0 + inv01 * b1;   // Floor σ̂ (m²)
        double s1 = inv01 * b0 + inv11 * b1;   // Rate  σ̂ (m²/s)
        if (!(s0 > kSigmaFloor)) { s0 = kSigmaFloor; ++out.floor_clamped; }
        if (!(s1 > 0.0))         { s1 = 0.0;         ++out.rate_clamped;  }
        out.sigma = {s0, s1};
        // Dispersion D(σ̂) = 2 N⁻¹ (Gaussian LS-VCE).  This is a formal std only:
        // rows within an epoch share a datum and are not independent, so it is an
        // optimistic lower bound, and the caller reports it as such.
        const double var0 = 2.0 * inv00;
        const double var1 = 2.0 * inv11;
        out.std = {var0 > 0.0 ? std::sqrt(var0) : 0.0,
                   var1 > 0.0 ? std::sqrt(var1) : 0.0};
        return out;
    }

    /// @return every (family, constellation) key accumulated so far (sorted).
    std::vector<std::pair<std::string, std::string>> keys() const {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(accum_.size());
        for (const auto& kv : accum_) {
            const std::string& k = kv.first;
            const auto sep = k.find('|');
            out.emplace_back(k.substr(0, sep),
                             sep == std::string::npos ? std::string() : k.substr(sep + 1));
        }
        return out;
    }

    double seedFloor() const { return seed_floor_; }
    double seedRate()  const { return seed_rate_;  }

    /// Parse a comma- or semicolon-separated component list ("pl,pb,cb") into
    /// families; spaces and tabs around each token are trimmed, empty tokens dropped.
    static std::vector<std::string> parseComponents(const std::string& csv) {
        std::vector<std::string> out;
        std::string tok;
        auto flush = [&] {
            // trim
            std::size_t a = tok.find_first_not_of(" \t");
            std::size_t b = tok.find_last_not_of(" \t");
            if (a != std::string::npos) {
                std::string t = tok.substr(a, b - a + 1);
                if (!t.empty()) out.push_back(t);
            }
            tok.clear();
        };
        for (char c : csv) {
            if (c == ',' || c == ';') flush();
            else tok.push_back(c);
        }
        flush();
        return out;
    }

private:
    /// One (family, constellation) sufficient-statistics block.
    struct Accum {
        double sum_log_e2 = 0.0;                   ///< Σ log(ê²), the robust geometric-mean accumulator.
        double sum_log_fv = 0.0;                   ///< Σ log(hand fold var) over the same rows (M2 v2).
        double n00 = 0.0, n01 = 0.0, n11 = 0.0;   ///< symmetric LS-VCE normal matrix N.
        double l0  = 0.0, l1  = 0.0;              ///< LS-VCE right-hand side l.
        double n_eff = 0.0;                        ///< forgetting-weighted row count.
        long long n_rows = 0;                      ///< undecayed total rows.
        void decay(double phi) {
            sum_log_e2 *= phi;
            sum_log_fv *= phi;
            n00 *= phi; n01 *= phi; n11 *= phi;
            l0  *= phi; l1  *= phi; n_eff *= phi;
        }
    };

    bool wants(const std::string& family) const {
        for (const auto& f : families_) if (f == family) return true;
        return false;
    }

    std::string key(const std::string& family, const std::string& sat) const {
        std::string c;
        if (split_by_const_ && !sat.empty()) c.assign(1, sat[0]);
        return family + "|" + c;
    }

    static constexpr double kRedundancyFloor = 1e-3;   ///< clamp the LS-VCE redundancy to [ε,1].
    static constexpr double kSigmaFloor      = 1e-14;  ///< positivity floor on Floor σ̂ (m²).
    static constexpr double kCondMax         = 1e6;    ///< data cond at or above ⇒ Rate frozen (floor-only solve).
    static constexpr double kSeedFloorFallback = 1e-6; ///< used if no config floor supplied (m²).
    static constexpr double kTinyE2          = 1e-20;  ///< floor on ê² inside the log (m²).
    // Geometric mean of a χ²₁ variate: exp(E[log χ²₁]) = exp(ψ(½)+ln2)
    //   = exp(−γ−ln2) = exp(−γ)/2 = 0.5614594836/2.  Divide geo_mean(ê²) by this
    // to recover the underlying innovation variance s.
    static constexpr double kChi2GeoMean     = 0.2807297418;

    std::vector<std::string> families_;
    bool   split_by_const_;
    double seed_floor_;
    double seed_rate_;
    double prior_rows_;
    double rate_scale_ = 0.0;
    double i0_floor_   = 0.0;
    double i0_rate_    = 0.0;
    double phi_        = 1.0;      ///< per-epoch forgetting factor (set in the constructor).
    bool   epoch_clean_ = true;
    double last_content_s_ = -1.0;
    std::map<std::string, Accum> accum_;
};

}  // namespace GnssProcessor
