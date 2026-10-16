#pragma once
/**
 * @file NetworkBlockBaarda.hpp
 * @brief BlockFormal: the outlier census over (station, satellite) LOS blocks, and its
 *        accumulator (docs/SPEC_redundancy_census.md, BB-INC-1..3).
 *
 * Measure-only, off by default, fingerprint-safe; with it off, the output is
 * byte-identical.  Nothing here changes a weight, a state or a decision.  SPEC §9
 * row 1 assigns any response to a loud block to a different card.
 *
 * ## Purpose of a block tier
 *
 * LASER Guide v2.7 slide 5-98: there is "not necessarily a one-to-one
 * correspondence between large residuals and outliers".  One physical fault on one
 * line of sight (multipath, a wet radome lobe, an unflagged slip) enters the code
 * and phase rows of every band at once, and the adjustment then spreads it across
 * all of them.  The producer's existing tiers sit on either side of that unit:
 * per-row w-tests below it, station-level median polish above it.  Neither can
 * name the line of sight that carried the fault.
 *
 * The primitive is @ref NetworkNormalEquations::blockResidualCofactor (INC-0), the
 * @f$b\times b@f$ residual cofactor @f$Q_{\tilde e,B}@f$ with its off-diagonals
 * kept.  This class computes the statistic and keeps its bookkeeping:
 * @f[
 *   T_B = \frac{\tilde e_B^\top Q_{\tilde e,B}^{-1} \tilde e_B}{b},
 *   \qquad t_B = \frac{T_B}{\hat\sigma_{0,f}^2}.
 * @f]
 * @f$T_B b@f$ is Baarda's multi-dimensional test statistic for the alternative
 * "an arbitrary bias vector on block B": with a diagonal weight matrix, the
 * @f$W_B@f$ factors of the general DIA form @f$\tilde e^\top Q_y^{-1}C(C^\top
 * Q_y^{-1}Q_{\tilde e} Q_y^{-1}C)^{-1}C^\top Q_y^{-1}\tilde e@f$ cancel.  For
 * @f$b=1@f$ it reduces to the scalar w-test, so the block tier generalizes the row
 * tier rather than running beside it.
 *
 * ## Reading the ladder: @f$t_B@f$ is a variance ratio, not a sigma
 *
 * The rungs are shared with the prior-residual census (@ref
 * NetworkPriorResidual::ladder, SPEC §4's "census bins"), so both cards write one
 * file format with one reader.  The unit differs.  In that census @f$|t|@f$ is a
 * standardized residual, and rung 4 means four sigma.  Here @f$T_B@f$ includes the
 * @f$1/b@f$, so @f$E[t_B]=1@f$ under @f$H_0@f$ for every block size, and rung 4
 * means the block carries four times the variance the model assigns it.  A
 * @c block_baarda rung-4 occupancy and a @c prior_residual rung-4 occupancy are
 * different quantities; the run-end log line states this too.
 *
 * ## Two corrections to SPEC §3's recipe
 *
 * SPEC §3 defines @f$\hat\sigma_{0,f}@f$ as a @ref NetworkCarryVce instance fed
 * @f$T_B@f$ per family with unit variance, following the D5 v2 pattern.  Taken
 * literally, that recipe is wrong in two ways, and each would produce a plausible
 * but mis-scaled ladder with no visible error:
 *
 * 1. **@ref NetworkCarryVce assumes one degree of freedom.**
 *    @c estimate() returns @c geo_mean(e^2)/kChi2GeoMean with
 *    @c kChi2GeoMean = 0.2807297418 = @f$2e^{\psi(1/2)}@f$, the geometric mean of
 *    a @f$\chi^2_1@f$ variate (NetworkCarryVce.hpp, @c kChi2GeoMean).  Under
 *    @f$H_0@f$, @f$T_B b \sim \sigma_0^2\chi^2_b@f$, and @f$2e^{\psi(b/2)}/b@f$ is
 *    0.281, 0.561, 0.763, 0.878 for @f$b=1,2,4,8@f$.  Feeding @f$\sqrt{T_B}@f$
 *    from a population of mixed @f$b@f$ therefore biases @f$\hat\sigma_0^2@f$ by
 *    up to about 3x, and the bias changes with the constellation's band plan.
 *    The correction is one factor: feed
 *    @f[ u_B = \sqrt{\;T_B\,b\;\frac{2e^{\psi(1/2)}}{2e^{\psi(b/2)}}\;} @f]
 *    (@ref blockChi2GeoMean), which is @f$\chi^2_1@f$-equivalent by construction,
 *    so @c robust_floor reads back as @f$\hat\sigma_0^2@f$ for any mix of block
 *    sizes.  At @f$b=1@f$ the D5 call stays byte-identical.
 * 2. **@c seed_floor_m2 is passed as 1.0 explicitly.**  NetworkCarryVce replaces a
 *    non-positive seed floor, including the constructor's default 0.0, with
 *    @c kSeedFloorFallback = 1e-6 m² (NetworkCarryVce.hpp).  A family that has not
 *    warmed up would then report @f$\hat\sigma_0 = 10^{-3}@f$ and inflate every
 *    @f$t_B@f$ in it by @f$10^6@f$.  The prior-residual census passes 1.0 for the
 *    same reason.
 *
 * The rest of the D5 v2 contract holds unchanged.  Predict, then update:
 * @ref beginEpoch freezes each family's @f$\hat\sigma_0@f$ through the previous
 * epoch before decaying the accumulator, so no block is standardized by a
 * calibration it contributed to.  No censoring: every checkable block feeds the
 * calibration before any threshold is consulted; feeding the above-threshold
 * blocks into @f$\hat\sigma_0@f$ is required, not merely harmless.
 * @f$\hat\sigma_0@f$ is not clamped.  D5 clamps because it acts on the value; a
 * measure-only census that clamped would report a family calibrated optimistically
 * as if it were calibrated.
 *
 * ## Uncheckable blocks
 *
 * @ref NetworkNormalEquations::blockResidualCofactor does not clamp, so a
 * fully-leveraged block comes back singular (the block analogue of
 * @f$r<\varepsilon@f$).  Such a block is counted with its identity and never
 * inverted (SPEC §3).  SPEC §3 allows a pseudo-inverse or a drop, counted either
 * way; this class drops: a pseudo-inverted @f$T_B@f$ has an
 * unknown effective number of degrees of freedom, so its rung would mean nothing,
 * and it would bias @f$\hat\sigma_0@f$ for every checkable block in its family.
 *
 * @warning On an unconverged run the uncheckable population is large and
 *          structural.  A free ambiguity absorbs its own arc's phase misfit
 *          exactly, so those post-fit phase residuals are identically zero and
 *          their rows have @f$r_i\approx 0@f$ (see NetworkRedundancyCensus.hpp).
 *          A block dominated by such rows is uncheckable by construction, not
 *          because of data quality.  SPEC §8's converged-warm-window rule
 *          addresses this.
 *
 * ## Isolation and testing
 *
 * A pure numeric leaf, like @ref NetworkCarryVce and @ref NetworkPriorResidual:
 * header-only, with no ApplicationNetwork coupling, no StateManager and no I/O.
 * It takes @ref Input blocks and returns ladders; @c tests/test_block_baarda.cpp
 * drives it directly with synthetic blocks of known truth.  The per-epoch trace,
 * event and aggregate emission is in @c filter/NetworkBlockBaarda.cpp, the only
 * translation unit of this card that touches traces.
 */

#include "common/GnssIdentity.hpp"       // systemOfSat, constellationKey: a satellite's family
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkMdb.hpp"           // netMdbConstIndex / netMdbConstChar: the shared constellation partition
#include "filter/NetworkPriorResidual.hpp" // ladder rungs and the label / corroboration enums reused per SPEC §4
#include "filter/NetworkRowClassify.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace GnssProcessor {

class NetworkNormalEquations;

/// Geometric mean of a @f$\chi^2_1@f$ variate: the constant hard-coded in
/// @ref NetworkCarryVce, repeated here so the rescaling in
/// @ref NetworkBlockBaarda::addBlock cancels exactly at @f$b=1@f$ (keeping the D5
/// call byte-identical) rather than to nine digits.
inline constexpr double kBlockChi2GeoMean1 = 0.2807297418;

/**
 * @brief Geometric mean of a @f$\chi^2_b@f$ variate, @f$2e^{\psi(b/2)}@f$.
 *
 * The scale correction that lets a population of mixed block sizes feed a
 * @ref NetworkCarryVce, whose own constant is the @f$b=1@f$ case.  Computed with
 * the recurrence @f$\psi(x+1)=\psi(x)+1/x@f$ from the closed forms
 * @f$\psi(1)=-\gamma@f$ and @f$\psi(1/2)=-\gamma-2\ln 2@f$, so it is exact for
 * every integer @p b; there is no table that could clamp.
 *
 * @param b Degrees of freedom (block size); @c b<=1 returns @ref kBlockChi2GeoMean1.
 */
inline double blockChi2GeoMean(int b) {
    if (b <= 1) return kBlockChi2GeoMean1;                 // 2 exp(psi(1/2))
    double psi = (b % 2 == 1) ? -1.9635100260214235        // psi(1/2) = -gamma - 2 ln 2
                              : -0.5772156649015329;       // psi(1)   = -gamma
    double x   = (b % 2 == 1) ? 0.5 : 1.0;
    const double target = 0.5 * static_cast<double>(b);
    while (x + 1.0 <= target + 1e-9) { psi += 1.0 / x; x += 1.0; }
    return 2.0 * std::exp(psi);
}

/// Largest block size counted individually in the size histogram; larger blocks
/// go to the overflow bucket.  SPEC §5 bounds a LOS block at b <= 8 (code and
/// phase times bands); the headroom makes a violation of that bound visible
/// instead of merging it into the b=8 cell.
inline constexpr int kBlockBaardaMaxB = 12;

/**
 * @brief Outlier census over (station, satellite) LOS blocks of the swept rows.
 *
 * One instance per producer run.  Unlike @ref NetworkPriorResidual (a local of the
 * run translation unit), it lives in @c NetworkCarryState, because it is fed inside
 * the redundancy census seam rather than in the driver; see the
 * @c [snapshot: EXEMPT] note at its declaration.
 */
class NetworkBlockBaarda {
public:
    /// Ladder rungs, shared with the prior-residual census (SPEC §4's "census
    /// bins") so both cards' CSVs have one shape and one reader.  The unit differs;
    /// see the file banner before comparing occupancies.
    static constexpr int kLadderBins   = NetworkPriorResidual::kLadderBins;
    static constexpr int kLadder4Sigma = NetworkPriorResidual::kLadder4Sigma;
    static const std::array<double, kLadderBins>& ladder() {
        return NetworkPriorResidual::ladder();
    }

    /// Calibration entries a family needs before its @f$\hat\sigma_0@f$ is trusted
    /// (here one entry per block).  Same value as D5, so the warm-up of D5, the
    /// prior-residual census and this census is one number.
    static constexpr double kWarmupRows = 200.0;

    /// One (family x label) exceedance ladder over @f$t_B@f$.  O(1) per block.
    struct Ladder {
        long long n_blocks      = 0;  ///< blocks folded into this ladder, uncheckable ones included.
        long long n_rows        = 0;  ///< data rows they span (the b-weighted denominator).
        long long n_uncheckable = 0;  ///< blocks whose @f$Q_{\tilde e,B}@f$ was singular or non-finite; counted, never inverted.
        long long n_raw_formal  = 0;  ///< blocks scaled with @f$\hat\sigma_0=1@f$ because the family had not warmed up, so @f$t_B \equiv T_B@f$.  Counted, not hidden.
        long long n_reported    = 0;  ///< blocks with @f$t_B >@f$ the report threshold.
        long long n_block_only  = 0;  ///< of those, blocks whose every row was individually below the same threshold: the population no existing tier sees (INC-3).
        std::array<long long, kLadderBins> bins{};  ///< cumulative exceedance: bins[k] = number of blocks with t_B > ladder()[k].
        double    sum_log_t    = 0.0; ///< sum of the log of the calibrated chi-square-1 equivalent (see @ref geoT), not of t_B.
        long long n_geo        = 0;   ///< blocks in @ref sum_log_t.
        /// Corroboration contingency (INC-2): row [0] = @f$t_B>@f$ threshold,
        /// row [1] = below; columns follow @ref PriorResidualCorrob.  All zero
        /// unless corroboration is armed.  The b-cell (row 0, Flat), loud blocks
        /// whose raw observables are flat, is the headline number.
        std::array<std::array<long long, kPriorResidualCorrobs>, 2> contingency{};

        // --- full identity of the worst block (invariant #4: a finding carries a name)
        double      max_t     = 0.0;  ///< largest @f$t_B@f$ seen.
        double      max_T     = 0.0;  ///< its raw @f$T_B@f$ (before @f$\hat\sigma_0@f$).
        double      max_row_t = 0.0;  ///< its loudest single row's @f$t_i@f$ (the row tier's view of the same block).
        int         max_b     = 0;    ///< its block size.
        int         max_station = -1; ///< its station index.
        std::string max_sat;          ///< its satellite.
        double      max_tow   = -1.0; ///< its TOW.
        bool        max_block_only = false;  ///< whether the worst block was invisible per row.

        /// Fraction of blocks above ladder rung @p k (0 when empty).
        double frac(int k) const {
            if (n_blocks <= 0 || k < 0 || k >= kLadderBins) return 0.0;
            return static_cast<double>(bins[static_cast<std::size_t>(k)])
                 / static_cast<double>(n_blocks);
        }
        /// Tail-robust calibration indicator: the geometric mean of the
        /// @f$\chi^2_1@f$-equivalent standardized block statistic.  A value of 1.0
        /// means the scale has converged, for any mix of block sizes.
        ///
        /// It is not the geometric mean of @f$t_B@f$ itself.  Under @f$H_0@f$,
        /// @f$E_{geo}[t_B] = 2e^{\psi(b/2)}/b@f$, which is 0.281 at @f$b=1@f$,
        /// 0.763 at @f$b=4@f$ and 0.878 at @f$b=8@f$, so a raw geometric mean over
        /// a mixed LOS population would move with the constellation's band plan and
        /// has no fixed reference.  Dividing the calibrated @f$\chi^2_1@f$
        /// equivalent by @ref kBlockChi2GeoMean1 removes that dependence, which
        /// makes this column comparable across families, runs and block sizes.  It
        /// plays the role of @c NetworkPriorResidual::Ladder::geoT2.
        double geoT() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t / static_cast<double>(n_geo)) / kBlockChi2GeoMean1;
        }
        /// block_only / blocks_flagged, the headline number of INC-3.  Per SPEC
        /// §8.4, this number decides whether a response card is worth writing.
        double blockOnlyFraction() const {
            if (n_reported <= 0) return 0.0;
            return static_cast<double>(n_block_only) / static_cast<double>(n_reported);
        }
        void reset() { *this = Ladder{}; }
    };

    /// One block as the feed site presents it.  Every value is raw (no
    /// @f$\hat\sigma_0@f$ applied): scaling is this class's job, so the feed site
    /// cannot standardize against the current epoch by mistake.
    struct Input {
        int              station = -1;    ///< station index (identity only).
        std::string_view sat;             ///< satellite, e.g. "G07": the second half of the block key and the source of the family.
        int              b = 0;           ///< block size (rows in the block).
        double           T = 0.0;         ///< @f$T_B = \tilde e_B^\top Q^{-1}\tilde e_B / b@f$.
        double           max_row_T = 0.0; ///< @f$\max_i \tilde e_i^2/Q_{ii}@f$ over the block's rows: the row tier's statistic on the same rows, in the same units as @ref T (the @f$b=1@f$ case), so the INC-3 comparison uses one scale.
        bool             uncheckable = false;  ///< @f$Q_{\tilde e,B}@f$ was singular or non-finite: count it, never invert it.
        int              n_code = 0;      ///< code rows in the block (composition, for the run-end readout).
        int              n_phase = 0;     ///< phase rows in the block.
        PriorResidualLabel label = PriorResidualLabel::Clean;  ///< assigned at the feed site; this class never labels.
    };

    /// Outcome of @ref addBlock, for the caller's per-epoch counters and events.
    struct BlockStat {
        bool   accepted   = false;  ///< entered a ladder (family live, inputs finite).
        bool   uncheckable = false; ///< counted as uncheckable; @ref t is meaningless.
        bool   raw_formal = false;  ///< the family had not warmed up: @f$\hat\sigma_0=1@f$.
        bool   reported   = false;  ///< @f$t_B@f$ cleared the report threshold.
        bool   block_only = false;  ///< reported, and every row individually quiet (INC-3).
        double t          = 0.0;    ///< @f$t_B@f$.
        double row_t      = 0.0;    ///< the loudest row's @f$t_i@f$.
        double sigma0     = 1.0;    ///< the frozen @f$\hat\sigma_0@f$ this block was scaled by.
        int    family     = -1;     ///< family index (constellation bin), -1 if not live.
        PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;
    };

    /**
     * @param families      Family names: constellation letters ("G", "E", ...).
     *                      SPEC §4 asks for ladders "per (family x label)".  The
     *                      prior-residual census's families are carry families
     *                      and have no meaning for a line of sight, so the
     *                      partition here is the constellation, the only axis that
     *                      labels a whole block (a block spans code and phase rows
     *                      by construction, so a row-class family would have no
     *                      single value).
     * @param window_epochs @f$\hat\sigma_0@f$ forgetting window.
     * @param min_rows      Warm-up bar (blocks, not rows) before a family's
     *                      @f$\hat\sigma_0@f$ is trusted.
     * @param report_sigma  @c network.block_baarda_report_sigma: the trace and event
     *                      threshold on @f$t_B@f$.  Affects reporting only.
     * @param corroborate   Join the slip-QC raw-observable verdict (INC-2).
     */
    NetworkBlockBaarda(std::vector<std::string> families, double window_epochs,
                       double min_rows = kWarmupRows, double report_sigma = 4.0,
                       bool corroborate = false)
        : families_(std::move(families)),
          // >= 0, not > 0: min_rows = 0 is a valid request for no warm-up bar
          // (calibrate from the first block), and the loader clamps the knob to
          // >= 0 for that reason.  Only a negative value is replaced by the
          // default.
          min_rows_(min_rows >= 0.0 ? min_rows : kWarmupRows),
          report_sigma_(report_sigma > 0.0 ? report_sigma : 4.0),
          corroborate_(corroborate),
          // The sigma0 accumulator.  split_by_const = false because the family
          // already is the constellation; a second split in NetworkCarryVce would
          // key every bucket "G|G" and change only the name.
          // seed_floor_m2 = 1.0 so an unwarmed family reads sigma0 = 1 exactly
          // (0.0 would become 1e-6; see the file banner, correction 2).
          inflation_(families_, window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0),
          state_(families_.size()) {}

    /// @return Whether any family is live (an unarmed instance is inert).
    bool enabled() const { return !families_.empty(); }
    const std::vector<std::string>& families() const { return families_; }
    double reportSigma() const { return report_sigma_; }
    bool   corroborating() const { return corroborate_; }

    /// @return Family index for @p sat, or -1 when its constellation was not
    ///         requested.  A family is named by its constellation's one-letter key
    ///         ("G", "E", ...).
    int familyIndex(std::string_view sat) const {
        if (sat.empty()) return -1;
        const std::string sys = constellationKey(systemOfSat(sat));
        for (std::size_t i = 0; i < families_.size(); ++i)
            if (families_[i] == sys) return static_cast<int>(i);
        return -1;
    }

    /// Install the raw-observable verdict resolver (INC-2).  Injected rather than
    /// passed through the call chain, as in the prior-residual census: the slip-QC
    /// channel is a local of the driver translation unit, and threading it through
    /// the census seam would add a diagnostic dependency to four signatures.
    void setCorroborator(std::function<PriorResidualCorrob(int, std::string_view)> fn) {
        corroborator_ = std::move(fn);
    }

    /**
     * @brief Predict step: freeze this epoch's @f$\hat\sigma_0@f$ and clear the
     *        per-epoch ladders.
     *
     * The order is the contract: every family's estimate is read (through the
     * previous epoch) before @ref NetworkCarryVce::beginEpoch decays the
     * accumulator, so a block is never standardized by a calibration it
     * contributed to.  @c clean is always true: a datum-changed epoch is a FRAME
     * population to measure, not one to drop (the prior-residual census passes
     * true for the same reason).
     */
    void beginEpoch(double content_time_s, double tow) {
        if (!enabled()) return;
        last_tow_ = tow;
        ++n_epochs_;
        for (std::size_t f = 0; f < state_.size(); ++f) {
            FamilyState& st = state_[f];
            const NetworkCarryVce::Estimate e =
                inflation_.estimate(families_[f], std::string());
            // Test n_eff, not robust_floor > 0: an empty bucket returns seed_floor_,
            // so robust_floor does not indicate whether data exists.  The explicit
            // n_eff > 0 keeps a min_rows = 0 family from reading warm before any
            // block has been folded (0 >= 0 is true), so a family with no
            // calibration data never reports itself calibrated.
            if (e.n_eff > 0.0 && e.n_eff >= min_rows_ && e.robust_floor > 0.0
                && std::isfinite(e.robust_floor)) {
                st.sigma0 = std::sqrt(e.robust_floor);
                st.warm   = true;
            } else {
                st.sigma0 = 1.0;      // raw-formal: t_B == T_B, and each block is flagged so
                st.warm   = false;
            }
            st.n_eff = e.n_eff;
            for (auto& l : st.epoch) l.reset();
        }
        ep_blocks_ = ep_uncheckable_ = ep_reported_ = ep_block_only_ = 0;
        ep_max_t_ = 0.0;
        inflation_.beginEpoch(content_time_s, /*clean=*/true);
    }

    /**
     * @brief Update step: fold one (station, satellite) block.
     *
     * @return The outcome, for the caller's events and counters.
     */
    BlockStat addBlock(const Input& in) {
        BlockStat out;
        if (!enabled()) return out;
        const int f = familyIndex(in.sat);
        if (f < 0) return out;                    // constellation not requested
        out.family = f;
        FamilyState& st = state_[static_cast<std::size_t>(f)];

        // Composition and size histogram come before any usability screen.  "Every
        // block had one row" and "the block tier found nothing" are different
        // findings, and invariant #4 requires that a reader can tell them apart.
        if (in.b > 0) {
            const std::size_t bi = static_cast<std::size_t>(
                std::min(in.b, kBlockBaardaMaxB + 1) - 1);
            ++b_hist_[bi];
            if (in.n_code > 0 && in.n_phase > 0) ++n_mixed_;
            else if (in.n_code > 0)              ++n_code_only_;
            else if (in.n_phase > 0)             ++n_phase_only_;
        }

        Ladder& L = st.epoch[static_cast<std::size_t>(in.label)];
        Ladder& R = st.run[static_cast<std::size_t>(in.label)];

        if (in.uncheckable) {
            ++L.n_uncheckable; ++R.n_uncheckable;
            ++L.n_blocks;      ++R.n_blocks;
            L.n_rows += in.b;  R.n_rows += in.b;
            ++ep_blocks_; ++ep_uncheckable_;
            out.accepted = true; out.uncheckable = true;
            return out;                            // never inverted, never calibrated
        }
        if (!(in.T >= 0.0) || !std::isfinite(in.T) || in.b <= 0) return out;

        // --- no censoring ----------------------------------------------------
        // The calibration is fed before any threshold is consulted, with a
        // chi-square-b to chi-square-1 rescaling so a mixed-b population is not
        // mis-scaled (file banner, correction 1).
        const double u2 = in.T * static_cast<double>(in.b)
                        * (kBlockChi2GeoMean1 / blockChi2GeoMean(in.b));
        if (u2 > 0.0 && std::isfinite(u2))
            inflation_.addRow(families_[static_cast<std::size_t>(f)], std::string(),
                              std::sqrt(u2), /*gap_s=*/0.0, /*post_var=*/0.0,
                              /*fold_var=*/1.0);

        const double s2 = st.sigma0 * st.sigma0;
        out.sigma0     = st.sigma0;
        out.raw_formal = !st.warm;
        out.t          = (s2 > 0.0) ? in.T / s2 : in.T;
        out.row_t      = (s2 > 0.0) ? in.max_row_T / s2 : in.max_row_T;
        out.accepted   = true;

        ++L.n_blocks;      ++R.n_blocks;
        L.n_rows += in.b;  R.n_rows += in.b;
        if (out.raw_formal) { ++L.n_raw_formal; ++R.n_raw_formal; }

        const auto& lad = ladder();
        for (int k = 0; k < kLadderBins; ++k) {
            if (!(out.t > lad[static_cast<std::size_t>(k)])) break;  // ascends: first miss ends it
            ++L.bins[static_cast<std::size_t>(k)];
            ++R.bins[static_cast<std::size_t>(k)];
        }
        // The geo channel takes the calibrated chi-square-1 equivalent, not t_B.
        // A raw geometric mean of t_B depends on b and cannot be read on a mixed
        // LOS population; see Ladder::geoT.
        const double un = (s2 > 0.0) ? u2 / s2 : u2;
        if (un > 0.0 && std::isfinite(un)) {
            const double lu = std::log(un);
            L.sum_log_t += lu; ++L.n_geo;
            R.sum_log_t += lu; ++R.n_geo;
        }

        out.reported = out.t > report_sigma_;
        // A block is block_only when it is loud and every one of its rows was
        // individually quiet at the same threshold in the same units.  No existing
        // tier sees that population, and SPEC §8.4 makes its fraction the number
        // that decides whether a response card is worth writing (INC-3, the
        // card's distinct output).
        out.block_only = out.reported && !(out.row_t > report_sigma_);
        if (out.reported) {
            ++L.n_reported; ++R.n_reported;
            ++ep_reported_;
            if (out.block_only) { ++L.n_block_only; ++R.n_block_only; ++ep_block_only_; }
        }

        if (corroborate_ && corroborator_) {
            out.corrob = corroborator_(in.station, in.sat);
            const std::size_t row = out.reported ? 0u : 1u;
            const std::size_t col = static_cast<std::size_t>(out.corrob);
            ++L.contingency[row][col];
            ++R.contingency[row][col];
        }

        auto keepMax = [&](Ladder& lg) {
            if (!(out.t > lg.max_t)) return;
            lg.max_t = out.t;   lg.max_T = in.T;   lg.max_row_t = out.row_t;
            lg.max_b = in.b;    lg.max_station = in.station;
            lg.max_sat.assign(in.sat.begin(), in.sat.end());
            lg.max_tow = last_tow_;
            lg.max_block_only = out.block_only;
        };
        keepMax(L);
        keepMax(R);

        ++ep_blocks_;
        if (out.t > ep_max_t_) ep_max_t_ = out.t;
        return out;
    }

    // --- readout ----------------------------------------------------------
    const Ladder& epochLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].epoch[static_cast<std::size_t>(l)];
    }
    const Ladder& runLadder(int f, PriorResidualLabel l) const {
        return state_[static_cast<std::size_t>(f)].run[static_cast<std::size_t>(l)];
    }
    double    sigma0(int f) const     { return state_[static_cast<std::size_t>(f)].sigma0; }
    bool      sigma0Warm(int f) const { return state_[static_cast<std::size_t>(f)].warm; }
    double    sigma0Neff(int f) const { return state_[static_cast<std::size_t>(f)].n_eff; }
    long long epochs() const          { return n_epochs_; }
    long long epochBlocks() const     { return ep_blocks_; }
    long long epochUncheckable() const{ return ep_uncheckable_; }
    long long epochReported() const   { return ep_reported_; }
    long long epochBlockOnly() const  { return ep_block_only_; }
    double    epochMaxT() const       { return ep_max_t_; }
    double    lastTow() const         { return last_tow_; }

    /// Run-cumulative block-size histogram; index k counts blocks with b = k+1,
    /// and the last cell is the "> kBlockBaardaMaxB" overflow.
    const std::array<long long, kBlockBaardaMaxB + 1>& sizeHistogram() const {
        return b_hist_;
    }
    long long runMixedBlocks() const     { return n_mixed_; }
    long long runCodeOnlyBlocks() const  { return n_code_only_; }
    long long runPhaseOnlyBlocks() const { return n_phase_only_; }

    /// Blocks over the whole run in one family, all labels.
    long long runBlocks(int f) const {
        long long n = 0;
        for (int l = 0; l < kPriorResidualLabels; ++l)
            n += runLadder(f, static_cast<PriorResidualLabel>(l)).n_blocks;
        return n;
    }
    long long runBlocksTotal() const {
        long long n = 0;
        for (std::size_t f = 0; f < families_.size(); ++f) n += runBlocks(static_cast<int>(f));
        return n;
    }

private:
    struct FamilyState {
        std::array<Ladder, kPriorResidualLabels> epoch{};
        std::array<Ladder, kPriorResidualLabels> run{};
        double    sigma0 = 1.0;   ///< frozen at beginEpoch (through the previous epoch).
        bool      warm   = false;
        double    n_eff  = 0.0;
    };

    std::vector<std::string> families_;
    double                   min_rows_;
    double                   report_sigma_;
    bool                     corroborate_;
    NetworkCarryVce          inflation_;
    std::vector<FamilyState> state_;
    std::function<PriorResidualCorrob(int, std::string_view)> corroborator_;

    double    last_tow_ = -1.0;
    long long n_epochs_ = 0;
    long long ep_blocks_ = 0, ep_uncheckable_ = 0, ep_reported_ = 0, ep_block_only_ = 0;
    double    ep_max_t_ = 0.0;
    std::array<long long, kBlockBaardaMaxB + 1> b_hist_{};
    long long n_mixed_ = 0, n_code_only_ = 0, n_phase_only_ = 0;
};

// ===========================================================================
//  Sweep and emission (filter/NetworkBlockBaarda.cpp)
// ===========================================================================

/// One reportable block, held until the emission seam writes its event row.  The
/// identity is what makes the finding usable (invariant #4): a ladder occupancy
/// without a name cannot be acted on or refuted.
struct BlockBaardaHit {
    int                 station = -1;
    std::string         sat;
    int                 b = 0;
    double              t = 0.0;       ///< @f$t_B@f$.
    double              row_t = 0.0;   ///< the loudest single row's @f$t_i@f$ (the row tier's view).
    double              T = 0.0;       ///< raw @f$T_B@f$ (before @f$\hat\sigma_0@f$).
    double              sigma0 = 1.0;
    int                 n_code = 0, n_phase = 0;
    bool                block_only = false;  ///< loud block, every row individually quiet (INC-3).
    bool                raw_formal = false;  ///< family had not warmed up.
    PriorResidualLabel  label  = PriorResidualLabel::Clean;
    PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;
};

/// Run-total tallies (standing invariant #4, statistical form).  Without
/// denominators, "the sweep ran and found nothing" cannot be told apart from "the
/// sweep never ran", so passes, station sweeps, blocks and the size histogram are
/// reported before any finding.  A run whose blocks were all @f$b=1@f$ produces a
/// ladder identical to the scalar w-test and an INC-3 fraction of 0 by
/// construction; @ref NetworkBlockBaarda::sizeHistogram distinguishes that case
/// from a real null result.
struct NetworkBlockBaardaTallies {
    long long passes         = 0;  ///< emission-seam invocations (whether the wiring executed at all).
    long long epochs         = 0;  ///< of those, passes whose epoch carried at least one block.
    long long station_sweeps = 0;  ///< per-station block sweeps performed.
    long long blocks         = 0;  ///< blocks enumerated.
    long long rows           = 0;  ///< data rows they spanned.
    long long unnamed_rows   = 0;  ///< swept data rows whose satellite could not be named, so they joined no block (an iono_sh_row is the usual cause); also incremented once per block whose post-fit residual is non-finite, which is then skipped.
    long long uncheckable    = 0;  ///< blocks whose cofactor could not be inverted; counted, identity kept.
    long long singular       = 0;  ///< of those, rejected as singular (conditioning or pivot test, or a negative or non-finite quadratic form).
    long long nonfinite      = 0;  ///< of those, rejected for a non-finite entry (a non-positive row weight).
    long long reported       = 0;  ///< blocks over the report threshold.
    long long block_only     = 0;  ///< of those, the population no row-level tier sees (INC-3).
    long long events         = 0;  ///< event rows emitted (rate-limited).
    long long no_resid       = 0;  ///< stations skipped because the post-fit residual could not be formed.
    double    wall_ms        = 0.0;///< run-total wall time inside the sweep and the emission.
    double    max_t          = 0.0;///< worst @f$t_B@f$ of the run.
    std::string max_id;            ///< its identity, "#STATION_INDEX/SAT b=… @tow …" (roster index, not name).
};

struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Sweep one station's (station, satellite) blocks: the costly tier of INC-1.
 *
 * Called from the redundancy census's swept branch (never from its free tier), so
 * it inherits the census's stride, targeted-escalation and trip schedule and adds
 * no scheduler of its own (SPEC §4).  Everything it needs is already in scope at
 * that site: the kernel, the station's slot, the hoisted datum-frame covariance,
 * the row classes and the per-row satellite produced by the census's classifier.
 *
 * Rows are grouped by satellite over data rows only.  Pins and coupling rows are
 * excluded because, on a within-epoch re-solve, the assembly's coupling rows are
 * re-whitened in a copy and no longer describe the system that was solved.
 *
 * @param acc             The run accumulator (@ref NetworkBlockBaarda::beginEpoch already called).
 * @param kernel          Solved arrowhead kernel (@ref NetworkNormalEquations::blockResidualCofactor).
 * @param slot            This station's kernel slot (@c est_index_of, never the roster index).
 * @param station_index   Roster index, for identity only.
 * @param R               The station assembly (row stack).
 * @param cls             Per-row classes from the census's classifier.
 * @param sat             Per-row satellite from the same classifier ("" = unnamed).
 * @param resid           Post-fit residuals @f$y - A_s x_s - A_r x_r@f$, in @c R.y's row order.
 * @param cov_free        Datum-frame shared covariance, built once by the census.
 * @param responded_sats  Satellites at this station on which the estimator has
 *                        already responded this epoch (a pending-reseed
 *                        ambiguity).  This is an estimator-side signal on purpose:
 *                        the INC-2 corroboration is the raw-observable side, and
 *                        if the label and the corroboration came from one source,
 *                        SPEC §8.2's positive control would be circular.
 * @param datum_changed   This epoch re-realized the datum, so the label is FRAME
 *                        unless the block is already RESPONDED.  RESPONDED takes
 *                        precedence, per SPEC §4's "union of its rows' labels" rule.
 * @param tallies         [in/out] run totals.
 * @param hits            [out] reportable blocks, appended.
 */
void networkBlockBaardaSweepStation(NetworkBlockBaarda& acc,
                                    const NetworkNormalEquations& kernel,
                                    std::size_t slot, int station_index,
                                    const StationAssembly& R,
                                    const std::vector<NetRowClass>& cls,
                                    const std::vector<std::string>& sat,
                                    const Eigen::VectorXd& resid,
                                    const Eigen::MatrixXd& cov_free,
                                    const std::set<std::string>& responded_sats,
                                    bool datum_changed,
                                    NetworkBlockBaardaTallies& tallies,
                                    std::vector<BlockBaardaHit>& hits);

/**
 * @brief Whether a block's residual cofactor is safe to invert.
 *
 * The uncheckable test of SPEC §3, exposed so the unit tests can drive it
 * directly.  @ref NetworkNormalEquations::blockResidualCofactor does not clamp, so
 * a fully-leveraged block arrives singular and is counted rather than inverted.
 *
 * @param Q          The @f$b\times b@f$ residual cofactor.
 * @param rcond_eps  Relative conditioning floor on the LDLᵀ pivots.
 * @param singular   [out] set when the conditioning test rejected it.
 * @param nonfinite  [out] set when an entry was not finite.
 */
bool blockCofactorIsCheckable(const Eigen::MatrixXd& Q, double rcond_eps,
                              bool* singular, bool* nonfinite);

/// Relative pivot floor for @ref blockCofactorIsCheckable.  A block whose smallest
/// LDLᵀ pivot is more than 10 decades below its largest is numerically
/// rank-deficient at double precision, and its @f$T_B@f$ would be dominated by
/// round-off in the direction the data does not constrain.
inline constexpr double kBlockCofactorRcondEps = 1e-10;

struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief Per-epoch emission: trace rows, event rows, epoch-status scalars.
 *
 * Called from the driver's SB_FRAMEDIAG block after @c networkRedundancyCensus,
 * which fed the accumulator this epoch.  The call is gated on
 * @c ctx.knobs.bb_on, so an unarmed run never enters this translation unit.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log),
 *            run.setup.stations (stations).
 * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.se (se).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx.
 *   - @p config_ = run.config.
 *   - @p log = run.log.
 *   - @p traces = run.traces.
 *   - @p se = ep.se.
 *   - @p stations = run.setup.stations.
 */
void networkBlockBaardaEmit(NetworkRun& run, const NetworkEpoch& ep);

/// Run-end readout (invariant #4): denominators first, then the ladders, then the
/// INC-3 fraction, which SPEC §8.4 makes the number that decides whether a response
/// card is worth writing.
void networkBlockBaardaRunEnd(const NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              const std::vector<NetStation>& stations);

/**
 * @brief The predict half of the block-Baarda census (BLOCKFORMAL): each family's sigma0
 *        through the previous epoch is frozen and the per-epoch ladders cleared before the
 *        census's rider feeds this epoch's blocks (card NOM3 step EP2).
 *
 * Under network block-Baarda (ctx.knobs.bb_on), its first line.  The body is the frozen driver
 * text, moved by scripts/nom_ep2_cut.py.
 *
 * @param run The run: ctx.carry.bb (written).
 * @param ep  The epoch, read: clock.pl_now_s, se.time.tow.
 */
void networkBlockBaardaPredict(NetworkRun& run, const NetworkEpoch& ep);
// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the BlockFormal LOS-block census.
 *
 * The table's row for the BlockFormal LOS-block census; the report is the overload
 * above.
 *
 * Gate: prints under `ctx.knobs.bb_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkBlockBaardaRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;

/**
 * @brief Seats the block-Baarda census and its corroboration resolver when the census is armed.
 *
 * A census arm.  An unarmed run keeps the default-constructed empty-family instance.  Setup
 * builder (card NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.ctx.carry.bb.
 */
void networkBlockBaardaArm(NetworkRun& run);

}  // namespace GnssProcessor
