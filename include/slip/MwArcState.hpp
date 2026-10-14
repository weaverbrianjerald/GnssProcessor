#pragma once

/**
 * @file MwArcState.hpp
 * @brief The MW slip detector's per-arc state and its constants table (namespace
 *        @c mw), internal to the detector.
 *
 * Owned by @c MwSlipDetector: the detector's private maps hold these structs, and only
 * slip/MwSlipDetector.hpp and the detector's own source files (src/core/slip/Mw*.cpp,
 * which open with `using namespace mw;`) name them.  The constants are fixed tuning
 * values, not configuration: every knob the detector reads is listed in the
 * constructor's log line and comes from @c GnssConfig.
 *
 *   - The constants table, grouped by the decision each constant serves.
 *   - @ref mw::MwArcStats: one MW arc's running statistics, windows, lock and pending
 *     TD confirmation.
 *   - @ref mw::DemotedArcLevel: the pre-jump level a demoted arc leaves for the
 *     across-gap settle estimate.
 *   - @ref mw::EstimatedBias: the in-house phase-bias estimate of one arc key.
 *   - @ref mw::BridgeArcStats: one tri-frequency phase-bridge arc (network-only).
 */

#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <string>

namespace GnssProcessor {

    /// The MW slip detector's constants table and per-arc state (see the file banner).
    namespace mw {

        // ======================================================================
        // The constants table
        // ======================================================================

        // --- Per-arc windows and maturity --------------------------------------------

        /** Maximum number of recent MW time-differences kept per arc
         *  (@c diff_window, @c td2_window) for MAD computation. */
        inline constexpr size_t kMwWindowSize = 30;

        /** Fixed capacity of the per-arc local-median ring buffer (raw MW
         *  values, meters).  The effective window length is the configured
         *  @c CycleSlipConfig::mw_local_window (≤ this cap; the config loader
         *  clamps).  A fixed @c std::array (rather than a deque) keeps the
         *  ring portion of @c MwArcStats trivially copyable, so the per-epoch
         *  @c arc_snapshots_ copy and the rollback in @c resetSatellite /
         *  @c resetSatelliteSignal stay cheap. */
        inline constexpr size_t kMwLocalWindowCap = 64;

        /** Maximum effective count used in the Hatch (recursive) mean.
         *  Once the arc exceeds this count the weighting denominator is
         *  clamped, giving new samples a minimum weight of 1/kMaxHatchCount.
         *  Without this cap the mean becomes nearly frozen on long arcs and
         *  cannot track level shifts that fall just below the detection
         *  threshold, producing the characteristic exponential-decay
         *  residual pattern reported in MW time-difference diagnostics. */
        inline constexpr size_t kMaxHatchCount = 100;

        /**
         * @brief Minimum number of diff_window entries before MAD-σ is
         *        trusted as the adaptive threshold input.  A MAD computed
         *        from only 5 diffs is statistically marginal: when those
         *        few diffs happen to be tight, the MAD collapses toward
         *        zero and the threshold falls from about one cycle to a
         *        small fraction of a cycle (seen in MWWL traces).  15 diffs
         *        give the MAD enough degrees of freedom to be
         *        representative.  During the warmup phase
         *        @c computeAdaptiveThreshold uses the max of the
         *        cross-sat MAD, a mode-specific per-arc spread term
         *        (Welford σ_emp in the level modes; the young TD MAD
         *        over the partial diff_window / td2_window in the
         *        TimeDifference modes; see the @c mode_sigma_m
         *        paragraph of @c computeAdaptiveThreshold), and the
         *        absolute @c kMinSigmaMeters floor, none of which
         *        require external calibration. */
        inline constexpr size_t kMinEpochsForAdaptive = 15;

        // --- Threshold floors and the cross-satellite pool ---------------------------

        /** Lower bound on the MAD-derived sigma (meters) used in
         *  @c computeAdaptiveThreshold.  Prevents the threshold from
         *  collapsing to near-zero when the diff_window happens to be
         *  unnaturally tight (e.g., a brief stretch of nearly-identical
         *  samples on a quiet, short arc).  Set to 1 cm, well below the
         *  typical L1/L2 WL MAD (~5–15 cm), so it does not change behavior
         *  for the standard pairs, while keeping the threshold
         *  wavelength-independent and useful for ultra-long EWL pairs
         *  (e.g., L5X/L8X at λ ≈ 19.5 m) whose natural MAD can be
         *  millimeters and whose meaningful anomalies live at the
         *  centimeter scale. */
        inline constexpr double kMinSigmaMeters = 0.01;

        /** Minimum cross-sat pool size before MAD is statistically
         *  meaningful.  Below it, the previous epoch's σ stays in place
         *  (or no entry exists, at cold start) instead of a value computed
         *  from a degenerately small pool. */
        inline constexpr size_t kMinCrossSatSamples = 5;

        // --- The wide-lane lock ------------------------------------------------------

        // Note: the WL lock criterion is an adaptive 3σ confidence test on
        // the running mean, not a fixed-N wait:
        //
        //     |frac| + 3 · σ_mean / λ_WL  <  0.5    and    |frac| < kWlMaxFrac
        //
        // implemented inline in @c processMwSample with a floor of
        // n ≥ kWlLockMinN (10).  Clean arcs lock at that floor; noisy arcs
        // wait until √n has averaged the noise down enough.  See the inline
        // kWlLockKSigma / kWlLockMinN constants there.

        /** Hard cap on the raw fractional residual |mean/λ_WL − round(...)|
         *  for an MW arc to lock (and so to be exposed to AR).  The
         *  statistical 3σ test alone can accept a non-integer running mean
         *  (e.g. 3.33 with σ_mean = 0.05) as well-determined and round it
         *  to a possibly wrong integer.  This cap treats a large raw |frac|
         *  as a real-bias signal that disqualifies the arc regardless of
         *  statistical confidence.  Applied by the lock criterion in
         *  @c processMwSample; after lock, @c kWlLockDriftMaxCyc bounds the
         *  drift, and @c getResolvedWlAmbiguities / @c getResolvedWlFractions
         *  do not re-apply this cap. */
        inline constexpr double kWlMaxFrac = 0.30;

        /** Live-mean drift threshold (WL cycles) that demotes a locked
         *  arc back to unlocked.  After lock the Welford / Hatch mean
         *  is not pinned to @c rounded_wl × λ_WL; it keeps tracking, so
         *  persistent disagreement between the running mean and the
         *  locked integer shows up as drift.  When |mean/λ − rounded_wl|
         *  exceeds this threshold, the accumulated evidence does not
         *  support the lock (either the initial lock was premature, or
         *  the arc has absorbed enough slipped samples to migrate), so
         *  the arc is demoted and re-resolves.  0.4 cycles is inside the
         *  rounding basin (< 0.5) and leaves headroom over the lock
         *  criterion's kWlMaxFrac = 0.30 ceiling, so a freshly locked arc
         *  with frac near the lock cap does not demote itself on the next
         *  sample. */
        inline constexpr double kWlLockDriftMaxCyc = 0.4;

        // --- Gaps and the across-gap settle estimate ---------------------------------

        /** Default maximum inter-epoch gap (seconds) before the arc is
         *  reset.  Override at runtime via @c setMaxGapSeconds; the typical
         *  use is to tighten this to 1.5 × the sampling interval when IMU
         *  is not available, so a brief outage resets the arc instead of
         *  silently absorbing a slip that occurred during the gap.
         *  When IMU is available and bridging gaps via INS propagation,
         *  the default is preserved so legitimate continuous arcs
         *  survive 60–120 s outages. */
        inline constexpr double kDefaultMaxGapSeconds = 120.0;

        /** Age limit (s) for a demoted-level stash. */
        inline constexpr double kDemotedLevelMaxAgeS = 120.0;
        /** Minimum post-restart samples before the settle estimate is trusted. */
        inline constexpr std::size_t kWlSettleMinEpochs = 10;

        // --- In-house bias estimation ------------------------------------------------

        /** Minimum arc count before an unbiased arc is eligible to commit
         *  a bias estimate.  Higher than @c kWlLockMinN because the commit
         *  is a one-shot integer decision (whereas the lock criterion has
         *  the k_σ statistical safety net that scales with √n). */
        inline constexpr size_t kBiasCommitMinCount = 20;

        /** Maximum |frac| (in cycles) for an unbiased arc to commit a
         *  bias.  Tighter than @c kWlMaxFrac because the commit decides
         *  which integer the arc converged to without external
         *  corroboration; a sat with frac in the 0.20–0.30 zone may
         *  legitimately belong to either side, and waiting for more
         *  evidence (or skipping the satellite) is preferred to guessing. */
        inline constexpr double kBiasCommitMaxFrac = 0.20;

        /** Maximum σ_mean (cycles) for commit.  Ensures the running mean
         *  is statistically well-determined before its residual is
         *  declared a hardware bias. */
        inline constexpr double kBiasCommitMaxSigmaCyc = 0.05;

        /** EWMA blending coefficient for cross-arc bias updates.  A small
         *  value (slow EWMA) reflects that hardware biases drift on
         *  hours-to-days timescales, so a single arc shouldn't move the
         *  estimate much; many arcs aggregating through the EWMA refine it. */
        inline constexpr double kBiasEwmaAlpha = 0.20;

        // --- The phase bridge (network-only) -----------------------------------------

        /** Gross-jump refloat guard for a bridge arc (m).  A WL/EWL cycle slip that the
         *  ambiguity reset funnel missed jumps @f$GF_b@f$ by ≥ @f$\lambda_{WL}\approx 0.86@f$ m
         *  (a 1-cycle WL slip), far above the differential-iono residue over an arc
         *  window (@f$\approx 0.44\cdot\nabla I_1@f$, sub-decimeter).  0.4 m sits between
         *  the two, so the guard resets the accumulator on a real slip but not on iono
         *  drift.  It is a secondary guard behind the funnel; the §3.4 bridge-σ guard
         *  remains the wrong-integer backstop in any case. */
        inline constexpr double kBridgeJumpResetM = 0.4;

        // ======================================================================
        // The per-arc state
        // ======================================================================

        /**
         * @brief Per-arc running statistics for the MW time series.
         *
         * Maintains a recursively updated mean and Welford variance estimate,
         * plus a rolling window of recent epoch-to-epoch MW differences used
         * to compute the Median Absolute Deviation (MAD) for robust outlier
         * detection. Using differences instead of raw values makes the MAD
         * measure the actual noise level rather than the spread around the
         * (arbitrary) ambiguity-dominated mean.
         *
         * Additionally holds a short ring buffer of recent accepted raw MW
         * values (@c mw_window) for the local-median detection reference
         * (@c MwDetectReference::LocalMedian).  Both windows admit only
         * non-slip epochs.  The ring is maintained in every reference mode
         * (it is cheap), so the trace can show the would-be local reference
         * even when another reference governs detection.  Because the ring
         * lives inside this struct, the gap reset (@c stats = MwArcStats{}),
         * the @c arc_snapshots_ rollback, and the erase-on-slip path keep it
         * consistent without extra code.  Code paths that shift the arc's
         * reference frame by a constant (the BSSD pivot S-transform and the
         * in-house bias commit re-anchor) must shift the ring contents
         * alongside @c mean / @c prev_mw (see @c shiftLocalWindow).
         */
        struct MwArcStats {
            double mean{ 0.0 };            /**< Running (Hatch) mean of MW values (meters). */
            double M2{ 0.0 };              /**< Welford accumulator for variance (m^2). */
            size_t count{ 0 };             /**< Number of MW samples accumulated. */
            /** Lagged snapshot of {mean, M2, count} as of the end of the previous
             *  processed epoch (copied at the top of the per-epoch statistics
             *  update).  @ref arcZdMean serves these, so a same-epoch consumer
             *  (the MW-WL layer, SPEC_mw_wl_card §3.3) never re-ingests the
             *  current epoch's observations (the guard against counting 1/n of
             *  the information twice).  Frame rule: every constant-shift
             *  re-reference of @c mean (BSSD pivot S-transform, in-house
             *  bias-commit re-anchor) shifts @c mean_lag by the same constant, so
             *  the lagged value is always expressed in the arc's current frame. */
            double mean_lag{ 0.0 };        /**< Lagged mean (meters); see block comment. */
            double M2_lag{ 0.0 };          /**< Lagged Welford accumulator (m^2). */
            size_t count_lag{ 0 };         /**< Lagged sample count. */
            double prev_mw{ 0.0 };         /**< Previous epoch MW value for differencing (meters). */
            /** The lagged raw sample: @c prev_mw as of the top of the current
             *  epoch's statistics update, i.e. the raw MW of the previous
             *  processed epoch, served by @ref arcZdLast with the same one-epoch
             *  lag (F1) as @c mean_lag.  Frame rule: every constant-shift
             *  re-reference of @c prev_mw (BSSD pivot S-transform, in-house
             *  bias-commit re-anchor) shifts this alongside.  NaN until two
             *  epochs have been processed. */
            double prev_mw_lag{ std::numeric_limits<double>::quiet_NaN() };
            double prev_epoch_s{ 0.0 };     /**< Continuous GPS seconds @c prev_mw was sampled (WL-3 alignment stamp). */
            double prev_epoch_lag_s{ 0.0 }; /**< Continuous GPS seconds @c prev_mw_lag was sampled (WL-3: the SD pivot re-add + the caller's cycle-consistency guard both align on this). */
            double last_epoch_s{ 0.0 };    /**< Continuous GPS seconds of the last update (for gap detection). */
            std::deque<double> diff_window; /**< Rolling window of epoch-to-epoch MW differences (meters). */
            bool   bia_corrected{ false };  /**< True if both satellite BIA bias and BSSD receiver bias are removed (integer-ready). */
            int    rounded_wl{ 0 };         /**< Rounded WL integer once locked (only valid when bia_corrected). */
            bool   wl_locked{ false };      /**< True once the WL integer has been locked by rounding. */
            double lambda_wl{ 0.0 };        /**< Wide-lane wavelength (meters) cached for slip repair. */
            /** The RINEX code observables this arc's MW combination was formed
             *  from, recorded per accepted sample so a consumer can name the
             *  satellite code-bias columns the observable carries (see
             *  @ref MwArcZdMean::code1).  The arc key holds only the two phase
             *  signals, so without these the code channel is unrecoverable and a
             *  consumer must guess it from the phase attribute.  Short RINEX-3
             *  codes ("C2W") fit in std::string's small buffer, so the per-epoch
             *  @c arc_snapshots_ copy stays allocation-free. */
            std::string code1, code2;
            double last_dev{ 0.0 };         /**< Active-reference deviation at the most recent epoch (meters). */
            double last_sigma{ std::numeric_limits<double>::quiet_NaN() }; /**< Active-reference sigma at the most recent epoch (meters). */
            std::array<double, kMwLocalWindowCap> mw_window{}; /**< Ring of recent accepted raw MW values (meters); valid entries are [0, mw_window_size). */
            size_t mw_window_head{ 0 };    /**< Next ring write slot (wraps at the configured window length). */
            size_t mw_window_size{ 0 };    /**< Number of valid ring entries (≤ configured window length). */
            /** Previous epoch's first time-difference TD1 = mw[k−1] − mw[k−2]
             *  (meters); NaN until two samples exist.  Combined with the
             *  current TD1 it yields the second time-difference
             *  TD2 = TD1[k] − TD1[k−1] for MwTdOrder::Second.  Updated every
             *  epoch alongside @c prev_mw, slip epochs included (see the
             *  echo note on @c td2_window).  Frame handling: the bias-commit
             *  re-anchor is a true constant shift within one sample series,
             *  to which TDs are invariant, so no fix-up is needed.  The BSSD
             *  pivot S-transform is not a constant shift of one series (it
             *  swaps which pivot's samples are differenced away), so the
             *  S-transform re-references this field by subtracting the new
             *  pivot's own old-frame prev_diff.  That is exact through the
             *  shared old-pivot terms, and it is required after a slip-forced
             *  handover, where the old pivot's dN would otherwise echo into
             *  every surviving arc's TD2 at the handover epoch. */
            double prev_diff{ std::numeric_limits<double>::quiet_NaN() };
            /** Rolling window of second time-differences (meters), the MAD
             *  source for the MwTdOrder::Second gate.  Mirrors @c diff_window
             *  (which holds TD1) in admission rule (non-slip epochs only) and
             *  cap (@c kMwWindowSize).  Echo note: left alone, a real slip on a
             *  locked arc produces a +dN·λ TD2 at the slip epoch and a −dN·λ
             *  echo one epoch later (prev_diff carries the slip-sized TD1
             *  forward), and a locked-arc noise outlier o echoes twice (+o,
             *  −2o, +o over three epochs).  Flagged epochs never enter this
             *  window.  In the TimeDifference modes the locked-arc confirmation
             *  (@c td_confirm_pending) erases the arc on a confirmed step and
             *  excises a declared outlier from the TD chain, so these echoes
             *  do not form there.  Unlocked arcs erase at the slip epoch, so
             *  the echoes never form. */
            std::deque<double> td2_window;
            /** Locked-arc TD confirmation state (TimeDifference modes only).
             *  A TD gate firing on a locked arc is not silently absorbed: the
             *  memoryless TD statistic flags a real step exactly once, so
             *  absorbing it would discard the only detection it produces and
             *  leave the stale locked integer exposed to AR for the dozen or so
             *  epochs until drift demotion.  Instead this flag is set and the
             *  decision is deferred by one epoch.  The next sample resolves it
             *  by comparing two level hypotheses: after a one-epoch noise
             *  outlier the level returns to the pre-jump track, and after a real
             *  step it stays on the post-jump track.  A step demotes the lock,
             *  emits a slip with an unknown integer (downstream reset) and
             *  erases the arc.  An outlier keeps the lock and is excised from
             *  the TD chain.
             *
             *  The confirmation never publishes a known integer, because a
             *  deferred result is one epoch late: (a) the pass-2 SD→ZD
             *  re-referencing would pair a flag-epoch SD jump with the
             *  resolution epoch's pivot ZD slip (the wrong frame), and a pivot
             *  slip would then produce confidently wrong integers on every
             *  clean locked peer; (b) TurboEdit would pair the integer with the
             *  resolution epoch's GF value, the sign-flipped echo of the real
             *  jump; and (c) the KF post-fit or TurboEdit may already have acted
             *  at the flag epoch, so an additive repair at k+1 risks a double
             *  mutation.  Reset is the only safe deferred action.
             *
             *  Frame guards: pending is neither entered nor resolved across a
             *  frame discontinuity.  A bia_corrected flip of the incoming sample
             *  falls back to the legacy absorb (the deviation is a frame
             *  artifact, not an event); a pivot ZD slip at the flag epoch takes
             *  an immediate unknown-integer slip instead of deferring, staying
             *  synchronous with the pass-2 re-referencing and CycleSlipManager's
             *  pivot-slip handling; and a pivot ZD slip at the resolution epoch
             *  makes the hypotheses unclassifiable and resolves the same way.
             *  While pending, the locked integer is withheld from
             *  getResolvedWlAmbiguities / getResolvedWlFractions (and
             *  getResolvedWlQuality).  Cleared by the pivot S-transform, because
             *  the stored quantities are frame-bound; reset, gap and erase clear
             *  it through the struct's normal lifecycle. */
            bool   td_confirm_pending{ false };
            /** Flagged level jump TD1 = mw[k] − reference (meters) at the
             *  pending epoch; the post-jump track is pre + jump. */
            double td_confirm_jump_m{ 0.0 };
            /** Pre-jump reference level (meters) the jump was measured
             *  against (algebraically mw[k] − jump, which stays correct
             *  even when the jump itself was an excised re-based TD). */
            double td_confirm_pre_mw{ 0.0 };
            /** Flag-epoch gated deviation (meters), written as the action
             *  trace row's active_dev so |active_dev|/threshold > 1 holds
             *  by construction on that row. */
            double td_confirm_dev_m{ 0.0 };
            /** Flag-epoch gate (meters).  Carried rather than recomputed at
             *  resolution: recomputation does not reproduce the firing gate
             *  for warmup-branch locks (the cross-sat EWMA floor updates
             *  every epoch and re-expands at the new elevation) nor for
             *  TD1-fallback flags inside TD2 order. */
            double td_confirm_thresh_m{ 0.0 };
            /** Flag-epoch detect_mode code (2 = TD1, 3 = TD2; a TD1-fallback
             *  flag inside TD2 order correctly tags 2). */
            int    td_confirm_mode{ 0 };
        };

        /** Demoted pre-jump level stash for the across-gap WL settle
         *  estimate (see @c getAcrossGapWlEvidence).  Written by the
         *  locked-arc TD confirm demote; shifted alongside the live arcs
         *  on SD re-anchors; erased by the per-sat / per-signal / full
         *  resets and by age (@c kDemotedLevelMaxAgeS). */
        struct DemotedArcLevel {
            double pre_level_m{ 0.0 };   /**< Pre-jump arc level (m, current SD frame). */
            double lambda_wl{ 0.0 };     /**< WL wavelength at demote time (m). */
            double epoch_s{ 0.0 };       /**< Demote epoch (continuous GPS s). */
            bool   bia_corrected{ false }; /**< Frame flag at demote time. */
        };

        /**
         * @brief Persistent per-(sat, signal-pair) phase-bias estimate.
         *
         * Used when @c GnssConfig::getInhouseBiasEstimation() is enabled
         * for arcs lacking external BIA bias correction.  An arc that
         * matures with its running mean within @c kBiasCommitMaxFrac of
         * an integer cycle and with σ_mean below @c kBiasCommitMaxSigmaCyc
         * commits its residual offset as a stored bias.  The stored bias
         * is subtracted from incoming MW samples on subsequent arcs of
         * the same (satellite, signal-pair) so the new arc accumulates
         * an integer-near mean from epoch one and is eligible for the
         * normal lock criterion.
         *
         * Lifetime is decoupled from @c MwArcStats: the bias survives
         * arc resets (a new arc inherits the stored bias; only @c resetAll
         * clears it), survives cycle slips (the integer changed but the
         * bias did not), and is S-transformed alongside arc means when the
         * MW pivot changes.
         */
        struct EstimatedBias {
            double bias_m{ 0.0 };           /**< Stored bias in meters; subtract from raw mw_m to get integer-near. */
            double bias_var_m2{ 0.0 };      /**< Uncertainty (variance) in m². */
            size_t commit_count{ 0 };       /**< Number of arcs that have contributed via EWMA. */
            double last_update_epoch_s{ 0.0 }; /**< Continuous GPS seconds of the most recent update. */
        };

        // --- Network-only: the phase bridge, owned with card NOM ------------------

        /**
         * @brief Per-arc Welford of the geometry-free WL↔EWL phase bridge
         *        @f$GF_b=\phi_{WL}-\phi_{EWL}@f$ (meters, ZD, phase-only).
         *
         * A deliberately lean mirror of @ref MwArcStats: Hatch/Welford mean, M2 and
         * count, with a one-epoch lag snapshot (the same-epoch double-count guard
         * that @ref arcZdMean also uses) and a gap reset.  No SD pivot, no BIA
         * correction, no detection statistic: @f$GF_b@f$ is a raw-phase ZD quantity
         * (its receiver and satellite biases cancel in the closed DD cycle the layer
         * forms), and the cycle-slip response comes from the same funnel that resets
         * the ambiguity.  @ref resetSatellite / @ref resetSatelliteSignal /
         * @ref resetAll erase the matching bridge arc, and @ref updateBridgeArc
         * guards against a missed reset with a gross-jump refloat.  Keyed
         * @c "sat_sig1_sig2_sig5".
         */
        struct BridgeArcStats {
            double mean{ 0.0 };            ///< Running Hatch mean of @f$GF_b@f$ (m).
            double M2{ 0.0 };              ///< Welford accumulator (m²).
            std::size_t count{ 0 };        ///< Sample count.
            double mean_lag{ 0.0 };        ///< Lagged mean as of the previous epoch's end (m).
            double M2_lag{ 0.0 };          ///< Lagged Welford accumulator (m²).
            std::size_t count_lag{ 0 };    ///< Lagged sample count.
            double prev_gf{ std::numeric_limits<double>::quiet_NaN() };     ///< Previous epoch's raw @f$GF_b@f$ sample (m); staging for the lane increment (WL-3).
            double prev_gf_lag{ std::numeric_limits<double>::quiet_NaN() }; ///< Lagged raw sample (the previous processed epoch's @c prev_gf), served by @ref arcBridgeLast (WL-3 §3.2 once-only use).
            double prev_epoch_s{ 0.0 };     ///< Continuous GPS seconds @c prev_gf was sampled.
            double prev_epoch_lag_s{ 0.0 }; ///< Continuous GPS seconds @c prev_gf_lag was sampled (the caller's cycle-consistency stamp).
            double last_epoch_s{ 0.0 };    ///< Continuous GPS seconds of the last update (gap detection).
            double lambda_wl{ 0.0 };       ///< WL wavelength c/(f1−f2) (m).
            double lambda_ewl{ 0.0 };      ///< EWL wavelength c/(f2−f5) (m).
            double iono_coeff{ 0.0 };      ///< GF_b iono sensitivity C_iono (see @ref MwBridgeMean::iono_coeff).
        };

    } // namespace mw

} // namespace GnssProcessor
