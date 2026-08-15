#pragma once
/**
 * @file NetworkBlockBaarda.hpp
 * @brief BlockFormal — the (station, satellite) LOS block outlier census
 *        (docs/SPEC_redundancy_census.md), BB-INC-1..3: the accumulator.
 *
 * Measure-only, default-OFF, OFF byte-identical, fingerprint-SAFE.  Nothing here
 * touches a weight, a state or a decision, and SPEC §9 row 1 pre-declares that
 * ANY response to a loud block belongs to a different card.
 *
 * ## Why a block tier exists at all
 *
 * LASER Guide v2.7 slide 5-98: there is "not necessarily a one-to-one
 * correspondence between large residuals and outliers".  ONE physical fault on
 * ONE line of sight — multipath, a wet radome lobe, an unflagged slip — lands on
 * every band's code and phase row at once and is then SMEARED by the adjustment
 * across all of them.  The producer's tiers straddle exactly that unit: per-row
 * w-tests sit below it, station-level median polish sits above it, and nothing
 * between them can say WHICH line of sight carried a fault.  That is the gap
 * that made the 78-station mass refloat unattributable.
 *
 * INC-0 landed the primitive (@ref NetworkNormalEquations::blockResidualCofactor,
 * the @f$b\times b@f$ residual cofactor @f$Q_{\tilde e,B}@f$ with the
 * off-diagonals kept).  This class is the statistic and its bookkeeping:
 * @f[
 *   T_B = \frac{\tilde e_B^\top Q_{\tilde e,B}^{-1} \tilde e_B}{b},
 *   \qquad t_B = \frac{T_B}{\hat\sigma_{0,f}^2}.
 * @f]
 * @f$T_B b@f$ is exactly Baarda's multi-dimensional test statistic for the
 * alternative "an arbitrary bias vector on block B" (the @f$W_B@f$ factors of the
 * general DIA form @f$\tilde e^\top Q_y^{-1}C(C^\top Q_y^{-1}Q_{\tilde e}
 * Q_y^{-1}C)^{-1}C^\top Q_y^{-1}\tilde e@f$ cancel against a diagonal weight
 * matrix), and @f$b=1@f$ reduces to the scalar w-test.  This is the strict
 * generalization of the row tier, not a parallel instrument.
 *
 * ## Reading the ladder — @f$t_B@f$ IS A VARIANCE RATIO, NOT A SIGMA
 *
 * The rungs are shared with the prior-residual census (@ref
 * NetworkPriorResidual::ladder, SPEC §4's "census bins") so the two cards' output
 * is one file format and one reader.  **The UNIT is not the same.**  That card's
 * @f$|t|@f$ is a standardized residual, so rung 4 means "four sigma".  Here
 * @f$T_B@f$ carries the @f$1/b@f$, so @f$E[t_B]=1@f$ under @f$H_0@f$ for EVERY
 * block size and rung 4 means "this block carries four times the variance the
 * model says it should".  Comparing a @c block_baarda rung-4 occupancy against a
 * @c prior_residual rung-4 occupancy as if they were the same quantity is a
 * category error; the run-end line says so in words for the same reason.
 *
 * ## Two corrections to SPEC §3's recipe, both load-bearing
 *
 * SPEC §3 says @f$\hat\sigma_{0,f}@f$ is "a @ref NetworkCarryVce instance fed
 * @f$T_B@f$ per family with unit variance — the D5 v2 pattern verbatim".  Taken
 * literally that is WRONG in two ways, and both would have produced a plausible,
 * silently mis-scaled ladder:
 *
 * 1. **@ref NetworkCarryVce is hard-wired to one degree of freedom.**
 *    @c estimate() returns @c geo_mean(e^2)/kChi2GeoMean with
 *    @c kChi2GeoMean = 0.2807297418 = @f$2e^{\psi(1/2)}@f$, the geometric mean of
 *    a @f$\chi^2_1@f$ variate (NetworkCarryVce.hpp:435).  But @f$T_B b \sim
 *    \sigma_0^2\chi^2_b@f$, and @f$2e^{\psi(b/2)}/b@f$ runs 0.281, 0.561, 0.763,
 *    0.877 for @f$b=1,2,4,8@f$ — so feeding @f$\sqrt{T_B}@f$ from a mixed-@f$b@f$
 *    population biases @f$\hat\sigma_0^2@f$ by up to 3x, and the bias MOVES with
 *    the constellation's band plan.  The fix is one multiplication: feed
 *    @f[ u_B = \sqrt{\;T_B\,b\;\frac{2e^{\psi(1/2)}}{2e^{\psi(b/2)}}\;} @f]
 *    (@ref blockChi2GeoMean), which is @f$\chi^2_1@f$-equivalent by construction,
 *    so @c robust_floor reads back as @f$\hat\sigma_0^2@f$ for any mix of block
 *    sizes.  @f$b=1@f$ leaves the D5 call byte-identical.
 * 2. **@c seed_floor_m2 must be 1.0, explicitly.**  The constructor's default of
 *    0.0 is silently replaced by @c 1e-6 (NetworkCarryVce.hpp:161), so a family
 *    that has not warmed up would report @f$\hat\sigma_0 = 10^{-3}@f$ and inflate
 *    every @f$t_B@f$ in it by @f$10^6@f$.  The prior-residual census passes 1.0
 *    for the same reason.
 *
 * The rest of the D5 v2 contract is honoured verbatim: PREDICT-THEN-UPDATE
 * (@ref beginEpoch freezes the through-previous-epoch @f$\hat\sigma_0@f$ per
 * family BEFORE decaying the accumulator, so no block can standardize itself
 * away), and NEVER-CENSOR (every checkable block feeds the calibration before any
 * threshold is consulted — feeding above-threshold blocks into @f$\hat\sigma_0@f$
 * is NECESSARY, not merely safe).  Nothing here clamps @f$\hat\sigma_0@f$: D5
 * clamps because D5 ACTS, and a measure-only census that clamped would report a
 * calibrated-optimistic family as if it were calibrated.
 *
 * ## The UNCHECKABLE block
 *
 * @ref NetworkNormalEquations::blockResidualCofactor deliberately does not clamp,
 * so a fully-leveraged block comes back SINGULAR — the block analogue of
 * @f$r<\varepsilon@f$.  Such a block is counted with its identity and NEVER
 * inverted through (SPEC §3).  This class DROPS rather than pseudo-inverts, and
 * that is a deliberate reading of the "pinv or drop — count either way" licence:
 * a pinv'd @f$T_B@f$ has an unknown effective dof, so its ladder rung would mean
 * nothing and it would pollute @f$\hat\sigma_0@f$ for every honest block in its
 * family.  An honest count beats a meaningless number.
 *
 * @warning The uncheckable population is LARGE and STRUCTURAL on an unconverged
 *          run: a free ambiguity absorbs its own arc's phase misfit exactly, so
 *          those post-fit phase residuals are identically zero and their rows'
 *          @f$r_i\approx 0@f$ (43 % of data rows on a cold FT63 start — see
 *          NetworkRedundancyCensus.hpp).  A block dominated by them is
 *          uncheckable BY CONSTRUCTION, not by data quality.  SPEC §8's
 *          converged-warm-window rule exists precisely for this.
 *
 * ## Isolation / testing
 *
 * Pure numeric leaf in the @ref NetworkCarryVce / @ref NetworkPriorResidual
 * mould: header-only, no ApplicationNetwork coupling, no StateManager, no I/O.
 * It takes @ref Input blocks and returns ladders; @c tests/test_block_baarda.cpp
 * drives it directly with synthetic blocks of known truth.  The per-epoch trace /
 * event / aggregate emission lives in @c filter/NetworkBlockBaarda.cpp, the only
 * translation unit here that knows about traces.
 */

#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkMdb.hpp"           // netMdbConstIndex / netMdbConstChar — one constellation partition, one truth
#include "filter/NetworkPriorResidual.hpp" // the ladder rungs + the LABEL / CORROB enums SPEC §4 says to reuse
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

/**
 * @brief Geometric mean of a @f$\chi^2_b@f$ variate, @f$2e^{\psi(b/2)}@f$.
 *
 * The scale correction that makes a mixed-block-size population usable in a
 * @ref NetworkCarryVce, whose own constant is the @f$b=1@f$ case.  Computed from
 * the recurrence @f$\psi(x+1)=\psi(x)+1/x@f$ off the two closed forms
 * @f$\psi(1)=-\gamma@f$ and @f$\psi(1/2)=-\gamma-2\ln 2@f$, so it is exact for
 * every integer @p b rather than a table lookup that would silently clamp.
 *
 * @param b Degrees of freedom (block size); @c b<=0 returns the @f$b=1@f$ value.
 */
/// Geometric mean of a @f$\chi^2_1@f$ variate — @ref NetworkCarryVce's own
/// hard-coded constant, repeated here so the rescaling below cancels EXACTLY at
/// @f$b=1@f$ (the D5 call must stay byte-identical) rather than to nine digits.
inline constexpr double kBlockChi2GeoMean1 = 0.2807297418;

inline double blockChi2GeoMean(int b) {
    if (b <= 1) return kBlockChi2GeoMean1;                 // 2 exp(psi(1/2))
    double psi = (b % 2 == 1) ? -1.9635100260214235        // psi(1/2) = -gamma - 2 ln 2
                              : -0.5772156649015329;       // psi(1)   = -gamma
    double x   = (b % 2 == 1) ? 0.5 : 1.0;
    const double target = 0.5 * static_cast<double>(b);
    while (x + 1.0 <= target + 1e-9) { psi += 1.0 / x; x += 1.0; }
    return 2.0 * std::exp(psi);
}

/// Largest block size tracked individually in the size histogram; anything
/// larger lands in the overflow bucket.  SPEC §5 bounds a LOS block at b <= 8
/// (code+phase x bands); the extra headroom exists so a violation of that bound
/// is VISIBLE rather than silently folded into the b=8 cell.
inline constexpr int kBlockBaardaMaxB = 12;

/**
 * @brief One (station, satellite) LOS block outlier census over the swept rows.
 *
 * One instance per producer run.  Unlike @ref NetworkPriorResidual (a run-TU
 * local) this lives in @c NetworkCarryState, because its feed site is inside the
 * redundancy census seam rather than in the driver — see the
 * @c [snapshot: EXEMPT] note at its declaration.
 */
class NetworkBlockBaarda {
public:
    /// Ladder rungs are SHARED with the prior-residual census (SPEC §4's
    /// "census bins") so the two cards' CSVs have one shape and one reader.
    /// Read the unit warning in the file banner before comparing occupancies.
    static constexpr int kLadderBins   = NetworkPriorResidual::kLadderBins;
    static constexpr int kLadder4Sigma = NetworkPriorResidual::kLadder4Sigma;
    static const std::array<double, kLadderBins>& ladder() {
        return NetworkPriorResidual::ladder();
    }

    /// Rows a family's calibration needs before @f$\hat\sigma_0@f$ is trusted.
    /// D5's value, kept so the three cards' warm-up behaviour is one number.
    static constexpr double kWarmupRows = 200.0;

    /// One (family x label) exceedance ladder over @f$t_B@f$.  O(1) per block.
    struct Ladder {
        long long n_blocks      = 0;  ///< blocks folded into this ladder.
        long long n_rows        = 0;  ///< data rows they span (the b-weighted denominator).
        long long n_uncheckable = 0;  ///< blocks whose @f$Q_{\tilde e,B}@f$ was singular / non-finite — counted, never inverted.
        long long n_raw_formal  = 0;  ///< blocks reported against @f$\hat\sigma_0=1@f$ because the family had not warmed up.  @f$t_B \equiv T_B@f$; flagged, never hidden.
        long long n_reported    = 0;  ///< blocks with @f$t_B >@f$ report threshold.
        long long n_block_only  = 0;  ///< INC-3: of those, blocks whose EVERY row was individually below the same threshold — the population invisible to every existing tier.
        std::array<long long, kLadderBins> bins{};  ///< CUMULATIVE exceedance: bins[k] = #blocks with t_B > ladder()[k].
        double    sum_log_t    = 0.0; ///< sum log of the CALIBRATED chi-square-1-equivalent (see @ref geoT), not of t_B.
        long long n_geo        = 0;   ///< blocks in @ref sum_log_t.
        /// INC-2 contingency: [0] = @f$t_B>@f$ threshold, [1] = below; columns
        /// follow @ref PriorResidualCorrob.  All-zero unless corroboration is
        /// armed.  The b-cell (row 0, Flat) is the number that matters.
        std::array<std::array<long long, kPriorResidualCorrobs>, 2> contingency{};

        // --- worst block's FULL identity (invariant #4: a finding needs a name)
        double      max_t     = 0.0;  ///< largest @f$t_B@f$ seen.
        double      max_T     = 0.0;  ///< its raw @f$T_B@f$ (pre-@f$\hat\sigma_0@f$).
        double      max_row_t = 0.0;  ///< its loudest single row's @f$t_i@f$ (the row tier's view of the same block).
        int         max_b     = 0;    ///< its block size.
        int         max_station = -1; ///< its station index.
        std::string max_sat;          ///< its satellite.
        double      max_tow   = -1.0; ///< its TOW.
        bool        max_block_only = false;  ///< whether the worst block was invisible per-row.

        /// Fraction of blocks above ladder rung @p k (0 when empty).
        double frac(int k) const {
            if (n_blocks <= 0 || k < 0 || k >= kLadderBins) return 0.0;
            return static_cast<double>(bins[static_cast<std::size_t>(k)])
                 / static_cast<double>(n_blocks);
        }
        /// Tail-robust CALIBRATION indicator: the geometric mean of the
        /// @f$\chi^2_1@f$-equivalent, standardized block statistic.  **1.0 means
        /// the scale has converged**, for ANY mix of block sizes.
        ///
        /// It is deliberately NOT the geometric mean of @f$t_B@f$ itself, which
        /// would be useless here: under @f$H_0@f$, @f$E_{geo}[t_B] =
        /// 2e^{\psi(b/2)}/b@f$ — 0.281 at @f$b=1@f$, 0.763 at @f$b=4@f$, 0.878 at
        /// @f$b=8@f$ (verified numerically) — so a raw geometric mean of a mixed
        /// LOS population would drift with the constellation's band plan and could
        /// never be read against a fixed reference.  Dividing the calibrated
        /// @f$\chi^2_1@f$-equivalent by @ref kBlockChi2GeoMean1 removes exactly
        /// that dependence, which is what makes this column comparable across
        /// families, runs and block sizes — the same role
        /// @c NetworkPriorResidual::Ladder::geoT2 plays there.
        double geoT() const {
            if (n_geo <= 0) return 0.0;
            return std::exp(sum_log_t / static_cast<double>(n_geo)) / kBlockChi2GeoMean1;
        }
        /// INC-3's headline: block_only / blocks_flagged.  SPEC §8.4 — "this
        /// single number decides whether a response card is ever worth writing".
        double blockOnlyFraction() const {
            if (n_reported <= 0) return 0.0;
            return static_cast<double>(n_block_only) / static_cast<double>(n_reported);
        }
        void reset() { *this = Ladder{}; }
    };

    /// One block, as the feed site presents it.  Everything here is RAW (no
    /// @f$\hat\sigma_0@f$ applied) — the scaling is this class's job, so the feed
    /// site cannot accidentally standardize against the current epoch.
    struct Input {
        int              station = -1;    ///< station index (identity only).
        std::string_view sat;             ///< satellite, e.g. "G07" — the block key's second half and the family's source.
        int              b = 0;           ///< block size (rows in the block).
        double           T = 0.0;         ///< @f$T_B = \tilde e_B^\top Q^{-1}\tilde e_B / b@f$.
        double           max_row_T = 0.0; ///< @f$\max_i \tilde e_i^2/Q_{ii}@f$ over the block's rows — the ROW tier's statistic on the same rows, same units as @ref T (the @f$b=1@f$ case), which is what makes INC-3's comparison apples-to-apples.
        bool             uncheckable = false;  ///< @f$Q_{\tilde e,B}@f$ was singular or non-finite: count it, never invert it.
        int              n_code = 0;      ///< code rows in the block (composition, for the run-end readout).
        int              n_phase = 0;     ///< phase rows in the block.
        PriorResidualLabel label = PriorResidualLabel::Clean;  ///< assigned at the FEED SITE — this class never labels.
    };

    /// What @ref addBlock did, for the caller's per-epoch counters and events.
    struct BlockStat {
        bool   accepted   = false;  ///< entered a ladder (family live, inputs finite).
        bool   uncheckable = false; ///< counted as uncheckable; @ref t is meaningless.
        bool   raw_formal = false;  ///< the family had not warmed up: @f$\hat\sigma_0=1@f$.
        bool   reported   = false;  ///< @f$t_B@f$ cleared the report threshold.
        bool   block_only = false;  ///< reported AND every row individually quiet (INC-3).
        double t          = 0.0;    ///< @f$t_B@f$.
        double row_t      = 0.0;    ///< the loudest row's @f$t_i@f$.
        double sigma0     = 1.0;    ///< the frozen @f$\hat\sigma_0@f$ this block was scaled by.
        int    family     = -1;     ///< family index (constellation bin), -1 if not live.
        PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;
    };

    /**
     * @param families      Family names — CONSTELLATION letters ("G", "E", ...).
     *                      SPEC §4 asks for ladders "per (family x label)"; the
     *                      prior-residual census's families are CARRY families
     *                      and mean nothing for a line of sight, so the partition
     *                      here is the constellation (the only axis that can
     *                      label a WHOLE block — a block spans code AND phase
     *                      rows by construction, so a row-class family would have
     *                      no well-defined value).
     * @param window_epochs @f$\hat\sigma_0@f$ forgetting window.
     * @param min_rows      Warm-up bar (blocks, not rows) before a family's
     *                      @f$\hat\sigma_0@f$ is trusted.
     * @param report_sigma  @c network.block_baarda_report_sigma — the trace /
     *                      event threshold on @f$t_B@f$.  REPORTING ONLY.
     * @param corroborate   INC-2: join the slip-QC raw-observable verdict.
     */
    NetworkBlockBaarda(std::vector<std::string> families, double window_epochs,
                       double min_rows = kWarmupRows, double report_sigma = 4.0,
                       bool corroborate = false)
        : families_(std::move(families)),
          // >= 0, not > 0: min_rows = 0 is a LEGITIMATE request for "no warm-up
          // bar, calibrate from the first block" (the loader clamps the knob to
          // >= 0 for exactly that reason).  Only a NEGATIVE value is a mistake
          // worth substituting a default for.
          min_rows_(min_rows >= 0.0 ? min_rows : kWarmupRows),
          report_sigma_(report_sigma > 0.0 ? report_sigma : 4.0),
          corroborate_(corroborate),
          // The sigma0 accumulator.  split_by_const = FALSE because the FAMILY
          // already IS the constellation — letting NetworkCarryVce split again
          // would key every bucket "G|G" and change nothing but the name.
          // seed_floor_m2 = 1.0 so an unwarmed family reads sigma0 = 1 exactly
          // (0.0 silently becomes 1e-6; see the file banner, correction 2).
          inflation_(families_, window_epochs, /*split_by_const=*/false,
                     /*seed_floor_m2=*/1.0, /*seed_rate_m2ps=*/0.0),
          state_(families_.size()) {}

    /// @return Whether any family is live (an unarmed instance is inert).
    bool enabled() const { return !families_.empty(); }
    const std::vector<std::string>& families() const { return families_; }
    double reportSigma() const { return report_sigma_; }
    bool   corroborating() const { return corroborate_; }

    /// @return Family index for @p sat, or -1 when its constellation was not
    ///         requested.  The satellite's leading character is the system.
    int familyIndex(std::string_view sat) const {
        if (sat.empty()) return -1;
        const std::string sys(1, sat.front());
        for (std::size_t i = 0; i < families_.size(); ++i)
            if (families_[i] == sys) return static_cast<int>(i);
        return -1;
    }

    /// INC-2: install the raw-observable verdict resolver.  Injected rather than
    /// plumbed, exactly as the prior-residual census does it — the slip-QC
    /// channel is a driver-TU local and threading it through the census seam
    /// would put a diagnostic dependency in four signatures.
    void setCorroborator(std::function<PriorResidualCorrob(int, std::string_view)> fn) {
        corroborator_ = std::move(fn);
    }

    /**
     * @brief PREDICT — freeze this epoch's @f$\hat\sigma_0@f$ and clear the
     *        per-epoch ladders.
     *
     * Order is the contract: every family's estimate is READ (through the
     * PREVIOUS epoch) before @ref NetworkCarryVce::beginEpoch decays the
     * accumulator, so a block is never standardized by a calibration it
     * contributed to.  @c clean is unconditionally true — a datum-changed epoch
     * is a FRAME population to MEASURE, not a population to drop (the
     * prior-residual census passes true for the same reason).
     */
    void beginEpoch(double content_time_s, double tow) {
        if (!enabled()) return;
        last_tow_ = tow;
        ++n_epochs_;
        for (std::size_t f = 0; f < state_.size(); ++f) {
            FamilyState& st = state_[f];
            const NetworkCarryVce::Estimate e =
                inflation_.estimate(families_[f], std::string());
            // n_eff, NOT robust_floor > 0: an empty bucket returns seed_floor_,
            // so robust_floor is never a proxy for "has data".  The explicit
            // n_eff > 0 is what keeps a min_rows = 0 family from reading "warm"
            // before a single block has been folded — 0 >= 0 is true, and a
            // family calibrated by nothing must never claim to be calibrated.
            if (e.n_eff > 0.0 && e.n_eff >= min_rows_ && e.robust_floor > 0.0
                && std::isfinite(e.robust_floor)) {
                st.sigma0 = std::sqrt(e.robust_floor);
                st.warm   = true;
            } else {
                st.sigma0 = 1.0;      // raw-formal: t_B == T_B, and every block says so
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
     * @brief UPDATE — fold one (station, satellite) block.
     *
     * @return What happened, for the caller's events and counters.
     */
    BlockStat addBlock(const Input& in) {
        BlockStat out;
        if (!enabled()) return out;
        const int f = familyIndex(in.sat);
        if (f < 0) return out;                    // constellation not requested
        out.family = f;
        FamilyState& st = state_[static_cast<std::size_t>(f)];

        // Composition + size histogram BEFORE any usability screen: "every block
        // had one row" and "the block tier found nothing" are different findings
        // and invariant #4 says a reader must be able to tell them apart.
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

        // --- NEVER CENSOR --------------------------------------------------
        // The calibration is fed BEFORE any threshold is consulted, and with a
        // chi-square-b -> chi-square-1 rescaling so a mixed-b population is not
        // silently mis-scaled (file banner, correction 1).
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
        // The geo channel takes the CALIBRATED chi-square-1-equivalent, not t_B —
        // see Ladder::geoT for why a raw geo-mean of t_B is b-dependent and
        // therefore unreadable on a mixed-LOS population.
        const double un = (s2 > 0.0) ? u2 / s2 : u2;
        if (un > 0.0 && std::isfinite(un)) {
            const double lu = std::log(un);
            L.sum_log_t += lu; ++L.n_geo;
            R.sum_log_t += lu; ++R.n_geo;
        }

        out.reported = out.t > report_sigma_;
        // INC-3, THE CARD'S UNIQUE DELIVERABLE.  A block is "block_only" when it
        // is loud AND every one of its rows was individually quiet at the SAME
        // threshold in the SAME units — that population is invisible to every
        // existing tier, and SPEC §8.4 makes its fraction the number that decides
        // whether a response card is ever worth writing.
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
        double    sigma0 = 1.0;   ///< frozen at beginEpoch (through the PREVIOUS epoch).
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
//  The sweep + emission (filter/NetworkBlockBaarda.cpp)
// ===========================================================================

/// One reportable block, held until the emission seam writes its event row.
/// Identity is the whole point (invariant #4): a ladder occupancy without a name
/// is a number nobody can act on or refute.
struct BlockBaardaHit {
    int                 station = -1;
    std::string         sat;
    int                 b = 0;
    double              t = 0.0;       ///< @f$t_B@f$.
    double              row_t = 0.0;   ///< the loudest single row's @f$t_i@f$ — the row tier's view.
    double              T = 0.0;       ///< raw @f$T_B@f$ (pre-@f$\hat\sigma_0@f$).
    double              sigma0 = 1.0;
    int                 n_code = 0, n_phase = 0;
    bool                block_only = false;  ///< INC-3: loud block, every row individually quiet.
    bool                raw_formal = false;  ///< family had not warmed up.
    PriorResidualLabel  label  = PriorResidualLabel::Clean;
    PriorResidualCorrob corrob = PriorResidualCorrob::NoEvidence;
};

/// Run-total tallies.  Standing invariant #4 in its statistical form: the
/// failure mode this card must be able to DISPROVE is "the sweep ran and found
/// nothing", which is indistinguishable from "the sweep never ran" without
/// denominators — so passes, station sweeps, blocks and the SIZE HISTOGRAM are
/// reported before any finding is.  In particular a run whose blocks were all
/// @f$b=1@f$ would produce a ladder identical to the scalar w-test and an INC-3
/// fraction of 0 BY CONSTRUCTION, not by finding; @ref NetworkBlockBaarda::sizeHistogram
/// is what makes those two cases distinguishable.
struct NetworkBlockBaardaTallies {
    long long passes         = 0;  ///< emission-seam invocations (did the wiring execute at all).
    long long epochs         = 0;  ///< …that carried at least one block.
    long long station_sweeps = 0;  ///< per-station block sweeps performed.
    long long blocks         = 0;  ///< blocks enumerated.
    long long rows           = 0;  ///< data rows they spanned.
    long long unnamed_rows   = 0;  ///< swept data rows whose satellite could not be named, so they joined NO block (an iono_sh_row is the usual cause).
    long long uncheckable    = 0;  ///< blocks whose cofactor could not be inverted — counted, identity kept.
    long long singular       = 0;  ///< …of those, rejected by the conditioning test.
    long long nonfinite      = 0;  ///< …of those, rejected for a non-finite entry (a non-positive row weight).
    long long reported       = 0;  ///< blocks over the report threshold.
    long long block_only     = 0;  ///< …of those, INC-3's invisible population.
    long long events         = 0;  ///< event rows emitted (rate-limited).
    long long no_resid       = 0;  ///< stations skipped because the post-fit residual could not be formed.
    double    wall_ms        = 0.0;///< run-total wall inside the sweep + emission.
    double    max_t          = 0.0;///< worst @f$t_B@f$ of the run.
    std::string max_id;            ///< its identity, "STATION/SAT b=… @tow …".
};

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Sweep one station's (station, satellite) blocks — INC-1's costly tier.
 *
 * Called from the redundancy census's SWEPT branch (never from its free tier), so
 * the stride / targeted-escalation / trip schedule is inherited exactly and this
 * card adds no scheduler of its own (SPEC §4).  Everything it needs is already in
 * scope there: the kernel, the station's slot, the hoisted datum-frame
 * covariance, the row classes and the per-row satellite the census's classifier
 * already produced.
 *
 * Rows are grouped by satellite over DATA rows only.  Pins and coupling rows are
 * excluded because on a within-epoch re-solve the assembly's coupling rows are
 * re-whitened in a copy and no longer describe the system that was solved.
 *
 * @param acc             The run accumulator (already @ref NetworkBlockBaarda::beginEpoch'd).
 * @param kernel          Solved arrowhead kernel (@ref NetworkNormalEquations::blockResidualCofactor).
 * @param slot            This station's kernel slot (@c est_index_of, never the roster index).
 * @param station_index   Roster index, for identity only.
 * @param R               The station assembly (row stack).
 * @param cls             Per-row classes from the census's classifier.
 * @param sat             Per-row satellite from the same classifier ("" = unnamed).
 * @param resid           POST-FIT residuals @f$y - A_s x_s - A_r x_r@f$, in @c R.y's row order.
 * @param cov_free        Datum-frame shared covariance, built once by the census.
 * @param responded_sats  Satellites at this station on which the estimator has
 *                        ALREADY responded this epoch (a pending-reseed
 *                        ambiguity).  Deliberately an ESTIMATOR-side signal: the
 *                        INC-2 corroboration is the RAW-observable side, and if
 *                        the label and the corroboration came from one source
 *                        SPEC §8.2's positive control would be circular.
 * @param datum_changed   This epoch re-realized the datum ⇒ FRAME (unless the
 *                        block is already RESPONDED — RESPONDED wins, which is
 *                        SPEC §4's "union of its rows' labels" rule).
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
 * @brief Is a block's residual cofactor safe to invert?
 *
 * The UNCHECKABLE test of SPEC §3, kept here so the unit tests can drive it
 * directly.  @ref NetworkNormalEquations::blockResidualCofactor deliberately does
 * not clamp, so a fully-leveraged block arrives singular and MUST be counted
 * rather than inverted through.
 *
 * @param Q          The @f$b\times b@f$ residual cofactor.
 * @param rcond_eps  Relative conditioning floor on the LDLᵀ pivots.
 * @param singular   [out] set when the conditioning test rejected it.
 * @param nonfinite  [out] set when an entry was not finite.
 */
bool blockCofactorIsCheckable(const Eigen::MatrixXd& Q, double rcond_eps,
                              bool* singular, bool* nonfinite);

/// Relative pivot floor for @ref blockCofactorIsCheckable.  A block whose
/// smallest LDLᵀ pivot is more than 10 decades under its largest is numerically
/// rank-deficient at double precision and its @f$T_B@f$ would be dominated by
/// round-off in the direction the data does not constrain.
inline constexpr double kBlockCofactorRcondEps = 1e-10;

/**
 * @brief Per-epoch emission — trace rows, event rows, epoch-status scalars.
 *
 * Called from the driver's SB_FRAMEDIAG block AFTER @c networkRedundancyCensus
 * (which is what fed the accumulator this epoch).  Gated on
 * @c ctx.knobs.block_baarda_on, so an unarmed run never enters this TU.
 */
void networkBlockBaardaEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces, const SyncedEpoch& se,
                            const std::vector<NetStation>& stations);

/// Run-end readout (invariant #4).  Denominators first, then the ladders, then
/// INC-3's fraction — the number SPEC §8.4 says decides whether a response card
/// is ever worth writing.
void networkBlockBaardaReport(const NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              const std::vector<NetStation>& stations);

}  // namespace GnssProcessor
