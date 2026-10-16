#pragma once

/**
 * @file MwSlipDetector.hpp
 * @brief Detects large wide-lane (MW) resets/slips using ionosphere-free combinations.
 *
 * The MwSlipDetector identifies large changes in MW-like observables between
 * epochs which typically indicate cycle slips or ambiguity resets. The detector
 * produces a list of ambiguity keys (e.g. "G01_L1C_L2W") that should be reset
 * when a large MW discontinuity is detected.
 *
 * @par Layout
 *   - slip/MwSlipTypes.hpp: the value types the detector returns (the per-ambiguity
 *     result, the query values).
 *   - slip/MwArcState.hpp: the per-arc state and the constants table (namespace
 *     @c mw), internal to the detector.
 *   - This header: the class.  Its user-side API, then its private members; the
 *     network-only surface is grouped at the end of the class behind one banner.
 *   - The bodies, by concern, in src/core/slip/: MwSlipDetector.cpp (construction and
 *     the per-epoch and per-sample drivers), MwSlipDetectorPairs.cpp (signal pairs and
 *     the MW combination), MwSlipDetectorStats.cpp (statistics, the adaptive threshold,
 *     the cross-satellite pool, the in-house bias), MwSlipDetectorQuery.cpp (resolved WL,
 *     active arcs, across-gap evidence, the worst score), MwSlipDetectorReset.cpp
 *     (external resets and the pivot hint), MwSlipDetectorTrace.cpp (the CSV trace), and
 *     the network-only MwSlipDetectorZd.cpp.
 *   - The two drivers' steps: MwSlipDetectorEpoch.cpp (detectLargeMwResets) and
 *     MwSlipDetectorSample.cpp (processMwSample), over the working values of
 *     slip/MwDetectorWork.hpp, which lists the steps in order.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "slip/ISlipDetector.hpp"
#include "slip/MwArcState.hpp"
#include "slip/MwSlipTypes.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @class MwSlipDetector
     * @brief Detector for large MW (wide-lane / MW-like) discontinuities.
     *
     * The detector computes an MW-like quantity in meters for each satellite
     * and signal pair and keeps running statistics per arc (keyed like
     * "G01_L1C_L2W").  It flags the pair's ambiguities for reset when a
     * sample's deviation from the arc reference exceeds an adaptive threshold:
     * k times the arc's MAD-based σ once the arc is mature, and a cross-satellite
     * or gross-slip floor before that.
     */
    class MwSlipDetector : public ISlipDetector {
    public:
        // --- The value types' names (defined once in slip/MwSlipTypes.hpp) -----
        using WlLockQuality       = MwWlLockQuality;        ///< @copydoc MwWlLockQuality
        using AcrossGapWlEvidence = MwAcrossGapWlEvidence;  ///< @copydoc MwAcrossGapWlEvidence
        using EpochPairStatus     = MwEpochPairStatus;      ///< Network-only; @copydoc MwEpochPairStatus
        using MwArcZdMean  = GnssProcessor::MwArcZdMean;   ///< Network-only; @copydoc GnssProcessor::MwArcZdMean

        /**
         * @brief Construct a new MwSlipDetector.
         * @param config Configuration providing thresholds and processing options.
         * @param log_manager Log manager used to emit diagnostic messages.
         */
        MwSlipDetector(const GnssConfig& config, LogManager& log_manager);

        // --- ISlipDetector contract -------------------------------------

        /**
         * @brief Detect slips for one epoch (ISlipDetector entry point).
         *
         * Runs @c detectLargeMwResets on @c context.observations and returns
         * the unified @c SlipEvent projection.  Each MW result is a band
         * difference (@f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$), so its event
         * carries @c SlipGranularity::BandDifference with both signals
         * populated.
         *
         * @note No code calls this.  @c CycleSlipManager skips the MW detector
         *       in its @c detect loop and consumes @c detectLargeMwResets
         *       directly, because it needs the full @c MwSlipResult WL-pair
         *       semantics that the projection does not preserve; every other
         *       driver holds the concrete type.  The override is kept because
         *       @c ISlipDetector declares @c detect pure virtual.
         *
         * @param context Epoch inputs (only @c observations is consulted).
         * @return Unified slip events for this epoch.
         */
        std::vector<SlipEvent> detect(const SlipEpochContext& context) override;

        /// @copydoc ISlipDetector::kind
        SlipDetectorKind kind() const override { return SlipDetectorKind::MelbourneWubbena; }

        /// @copydoc ISlipDetector::name
        std::string name() const override { return "MelbourneWubbena"; }

        /// @copydoc ISlipDetector::asMelbourneWubbena
        MwSlipDetector* asMelbourneWubbena() override { return this; }

        /**
         * @brief Detect large MW resets for the provided epoch observations.
         *
         * Iterates provided observations and returns per-ambiguity slip results.
         * Each result contains the ambiguity key and, for arcs where the WL was
         * locked to an integer, the integer slip magnitude dN_WL.  When a
         * satellite has 3+ frequencies, the caller can use multi-frequency
         * MW pair cross-consistency to identify which individual signal
         * slipped and perform a targeted single-signal ambiguity reset.
         *
         * @param observations Vector of per-satellite PppObservation for the epoch.
         * @return std::vector<MwSlipResult> Slip results for each affected ambiguity.
         */
        std::vector<MwSlipResult> detectLargeMwResets(const std::vector<PppObservation>& observations);

        /**
         * @brief Get all currently locked WL integer ambiguities.
         *
         * Returns a snapshot of every arc that has a locked WL integer value.
         * The key matches the MW arc stats key (e.g. "G01_L1C_L2W" for
         * uncombined). The value is the rounded SD WL integer.
         *
         * Useful for external components (e.g. AmbiguityResolver) that want
         * to constrain or validate narrow-lane ambiguities.
         *
         * @return Map of stats_key -> rounded WL integer.
         */
        std::map<std::string, int> getResolvedWlAmbiguities() const;

        /**
         * @brief Arc keys whose MW observable was affirmatively clean this epoch.
         *
         * An arc enters the set when, during the current
         * @c detectLargeMwResets pass, it (a) is mature (sample count ≥ 10; a
         * finite adaptive threshold is not used as a maturity proxy, because the
         * warm-up and cold-start branches return finite thresholds for 2-sample
         * arcs), (b) did not flag, (c) has an empirical MAD σ that gives the test
         * discriminating power (3σ ≤ 0.7·λ_WL, so noise can pull a real ±1-cycle
         * wide-lane slip inside the clean bound only as a ≥3σ event), and (d) had
         * a sample deviation from the arc's Hatch mean within
         * 0.3·@f$\lambda_{WL}@f$, so the wide-lane integer is demonstrably
         * unchanged at this epoch.  Key format matches the arc stats key
         * ("G01_L1C_L2W").
         *
         * Consumed by @c TurboEditResolver::resolve to gate its GF-only
         * equal-slip (@f$\Delta N_{WL}=0@f$) split.  MW silence is not sufficient
         * evidence for that split: a wl-locked arc deliberately absorbs a real
         * slip into its Welford stats without emitting a result, and an immature
         * arc could not have flagged at all.  Only affirmative cleanliness
         * justifies the equal-slip assumption.
         *
         * Cleared and rebuilt by every @c detectLargeMwResets call; contents
         * are valid for the current epoch only.
         *
         * @return Set of affirmatively-clean arc keys for the current epoch.
         */
        const std::set<std::string>& getEpochCleanPairs() const { return epoch_clean_pairs_; }

        /**
         * @brief Get the worst-case fractional residual of each
         *        currently-locked WL arc, in cycles.
         *
         * Reports @c |mean/λ_WL − rounded_wl| + 3·σ_mean/λ_WL, the same
         * worst-case-frac expression the lock criterion uses.  It captures
         * both the observed drift between the live running mean and the
         * locked integer and the statistical uncertainty of the mean.
         *
         * Companion to @c getResolvedWlAmbiguities() used by AR to gate
         * low-confidence locks (see @c AmbiguityResolverConfig::wl_max_frac).
         * Only arcs with @c wl_locked=true and a finite @c lambda_wl are
         * reported.  Frac of 0.0 = mean sits exactly on integer × λ with
         * zero variance; values approaching @c kWlLockDriftMaxCyc (~0.4)
         * indicate the arc is close to drift demotion.  Values cannot
         * exceed @c kWlLockDriftMaxCyc + 3·σ_mean because @c processMwSample
         * demotes any arc whose live drift crosses that threshold.
         *
         * @return Map of stats_key -> worst-case fractional residual (cycles).
         */
        std::map<std::string, double> getResolvedWlFractions() const;

        /**
         * @brief Per-arc standard error of the locked WL arc mean, in cycles.
         *
         * Same population and key space as @ref getResolvedWlAmbiguities and
         * @ref getResolvedWlFractions, and the same `σ_mean = √(M2/(n−1)/n) / λ_WL`
         * the fractions accessor forms internally, exposed on its own instead of
         * folded into the single worst-case-frac scalar.
         *
         * It is exposed separately because `wl_max_frac` is a point test on the
         * running residual: it says the locked integer looks consistent now.  It
         * cannot bound the probability that the integer is wrong, which is what
         * an acceptance decision must control.  Given σ_mean, one arc's rounding
         * success rate is `erf(1/(2√2·σ))`, the same per-component term
         * @ref ils::computeBootstrapSuccessRate uses on the LAMBDA tier, so
         * exposing σ lets the WL-only AR path run the same fixed-failure-rate
         * gate as the LAMBDA path instead of having none.
         *
         * A BSSD MW pivot is 0 cycles against itself by construction and is
         * reported with @c exact=true (success 1 regardless of maturity), matching
         * the 0.0 the fractions accessor reports for those keys.
         *
         * @c count travels with σ because a 2-sample arc whose samples happen to
         * agree yields σ ≈ 0 and a success rate ≈ 1 on one degree of freedom.  A
         * caller applying an FFR gate must also require maturity, or a meaningless
         * variance buys a fix.  Maturity is not gated here on purpose: that
         * threshold is consumer policy, not a detector constant.
         *
         * @return Map of stats_key -> lock quality.
         */
        std::map<std::string, WlLockQuality> getResolvedWlQuality() const;

        /**
         * @brief Get all mature, non-slipped MW arc keys.
         *
         * Returns every stats key that has an active MW arc with enough
         * epoch history (diff_window >= kMinEpochsForAdaptive) to have
         * detected a slip if one occurred.  Immature arcs are excluded
         * because their slip detection is deferred: treating them as
         * "clean" would create false constraints in the multi-frequency
         * solver.  An unlocked arc that flags a slip is erased at once and
         * drops out.  A locked arc that flags stays listed: the level modes
         * absorb the sample (drift demotion later unlocks the arc without
         * erasing it), and the TimeDifference modes normally erase it one
         * epoch later, when the step is confirmed.
         *
         * The key format is "SAT_SIG1_SIG2" (e.g. "G01_L1C_L2W").
         * This is used by CycleSlipManager to build clean-pair references
         * (dN_i - dN_j = 0) for multi-frequency cross-consistency analysis,
         * which works whether or not BIA satellite biases are available.
         *
         * @return Vector of mature active arc stat keys.
         */
        std::vector<std::string> getActiveArcKeys() const;

        /**
         * @brief Across-gap WL evidence for every recently-demoted arc.
         *
         * One entry per arc key ("G03_L1C_L2W") whose locked-arc demote
         * stashed a pre-jump level and whose post-restart arc is alive in a
         * comparable frame (same @c bia_corrected state; SD re-anchors
         * shift the stash alongside the live arcs).  Entries age out after
         * @c kDemotedLevelMaxAgeS.  Cheap (map walk); call once per epoch.
         *
         * @param epoch_now_s Continuous GPS seconds of this epoch (age gate).
         * @return Map from arc key to the settling WL evidence.
         */
        std::map<std::string, AcrossGapWlEvidence>
        getAcrossGapWlEvidence(double epoch_now_s) const;

        /**
         * @brief Roll back or clear the MW arcs of a specific satellite.
         *
         * Call this when an ambiguity is reset by an external mechanism (e.g.,
         * KF outlier handler).  Each of the satellite's arcs is restored to the
         * snapshot taken before this epoch's sample when one exists (the
         * accumulated noise statistics are kept and the WL lock is broken) and
         * erased otherwise.  Without this call, the sample carrying a slip that
         * the MW test missed would stay in the running statistics and mask the
         * slip.  The satellite's demoted-level stashes are dropped, and an SD
         * pivot entry held by the satellite is cleared so that a new pivot is
         * selected next epoch.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite) override;

        /**
         * @brief Roll back or clear the MW arcs that involve a specific
         *        (satellite, signal), leaving arcs on signal pairs that do
         *        not include this signal intact.
         *
         * Finer-grained alternative to @c resetSatellite for the common case
         * where only one signal slipped on a multi-frequency satellite (e.g.
         * L1C slipped while L2W and L5X are clean: the L2W/L5X arc survives
         * while the L1C/L2W and L1C/L5X arcs are rolled back or cleared).
         * Rollback and erase follow @c resetSatellite; demoted-level stashes
         * that involve the signal are dropped.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    Signal identifier (e.g. "L1C", "L2W", "L5X").
         */
        void resetSatelliteSignal(const std::string& satellite,
                                   const std::string& signal) override;

        /**
         * @brief Hint that a satellite should be deprioritized when picking
         *        the next MW pivot.
         *
         * Consumed by the next @c detectLargeMwResets call: the
         * elevation-based pivot selector skips listed satellites unless
         * they are the only candidate in a (constellation, signal-pair)
         * group.  After consumption the hint set is cleared, so the
         * caller must re-issue each epoch the deprioritization should
         * hold.
         *
         * Intended use: when an upstream component (typically the KF
         * post-fit residual detector via @c PivotManager::requestForcedHandover)
         * flags that the current KF pivot has slipped, it forwards the
         * same satellite here.  Without this hint the MW pivot picker
         * (which sees neither the KF residual nor the handover request)
         * can re-elect the slipped sat and propagate stale arcs into
         * the next epoch's MW frame.
         *
         * @param satellite Satellite identifier (e.g. "G27").
         * @param reason    Free-text reason logged at info level.
         */
        void notifyPivotForcedHandover(const std::string& satellite,
                                       const std::string& reason);

        /**
         * @brief Clear all per-arc state.
         *
         * Drops every entry in the running-statistics map and any SD pivot
         * bookkeeping.  Intended for hard-gap restarts: when a data-gap
         * threshold is exceeded, neither pre-gap MW values nor pre-gap
         * SD pivot choices are reliable indicators for the post-gap arcs,
         * so the safest restart is a full clear rather than per-sat resets.
         * The next call to @c detectLargeMwResets() then begins fresh
         * arcs with no slip flags fired against pre-gap history.  Called on
         * the concrete type by the baseline driver.
         */
        void resetAll();

        /**
         * @brief Enable CSV tracing for MWWL diagnostics.
         *
         * Opens or creates the file "mwwl_trace.csv" in the specified directory.
         * Subsequent calls to detectLargeMwResets() will write per-satellite MW
         * observations to this file for offline visualization.  Called on the
         * concrete type (@c CycleSlipManager::enableMwTracing, the real-time
         * and baseline drivers).
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened, false otherwise.
         */
        bool enableTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Provide satellite bias access for WL bias correction.
         *
         * When set, the detector subtracts the satellite MW bias computed
         * from BIA phase+code entries before accumulating the Hatch mean.
         * This makes the bias-corrected MW near-integer in WL cycles,
         * enabling deterministic rounding of the wide-lane ambiguity.
         *
         * @param corrections Pointer to GnssCorrections (may be nullptr to disable).
         */
        void setCorrections(const GnssCorrections* corrections) override { corrections_ = corrections; }

        /**
         * @brief Override the inter-epoch gap tolerance for arc resets.
         *
         * Overrides @c kDefaultMaxGapSeconds for this detector instance.
         * Application sets this to 1.5 × the effective sampling interval
         * (the RINEX interval, or processing.gnss_rate_s when set) when that
         * interval is coarser than 5 s and IMU is not available, so a missed
         * epoch resets the arc rather than silently absorbing a
         * slip-during-gap into the running mean.
         * When IMU bridges gaps the default 120 s is preserved by not
         * calling this setter.
         *
         * @param seconds  New gap tolerance.  Values ≤ 0 are ignored
         *                 (default is preserved).
         */
        void setMaxGapSeconds(double seconds);

        /**
         * @brief Query the worst MW deviation score for a satellite.
         *
         * Returns the maximum |deviation / sigma| across all MW arcs for
         * the given satellite, using the deviation and MAD-sigma from the
         * most recently processed epoch.  A higher score indicates a
         * stronger MW disturbance, making this satellite a better
         * candidate for ambiguity reset when multiple phase outliers
         * compete for the worst standardized residual.
         *
         * Returns 0 when no mature arcs exist for the satellite.
         *
         * @param satellite Satellite identifier (e.g. "E18").
         * @return Worst-case |dev/sigma| score (dimensionless).
         */
        double getWorstMwScore(const std::string& satellite) const;

        private:
        const GnssConfig& config_;    /**< Reference to processing configuration. */
        LogManager& log_manager_;     /**< Reference to logging manager. */
        PppUtils ppp_utils_;          /**< Utility helpers for frequency/wavelength lookups. */
        const GnssCorrections* corrections_{ nullptr }; /**< Optional bias lookup (owned externally). */

        /// Effective max-gap tolerance.  Overridable via @c setMaxGapSeconds;
        /// initialized to @c kDefaultMaxGapSeconds.
        double max_gap_seconds_{ mw::kDefaultMaxGapSeconds };

        /** Per-arc MW statistics, keyed by arc key (e.g. "G01_L1C_L2W"). */
        std::map<std::string, mw::MwArcStats> mw_stats_;

        /** Pre-update snapshot of each arc taken at the start of every
         *  @c processMwSample call (after gap detection, before any
         *  state-modifying processing).  Keyed identically to
         *  @c mw_stats_.  Consumed by @c resetSatellite /
         *  @c resetSatelliteSignal to roll back a single epoch's
         *  contribution when the KF post-fit residual check identifies
         *  a slip that the MW test missed.  The rollback keeps the arc's
         *  pre-slip noise statistics (mean, M2, diff_window, count)
         *  instead of wiping the arc and forcing a re-warmup.  It also
         *  clears @c wl_locked / @c rounded_wl, because a KF-detected slip
         *  means the underlying ambiguity changed and the locked integer
         *  is invalid.
         *
         *  Cleared at the top of @c detectLargeMwResets each epoch so
         *  the map only holds current-epoch snapshots.  Consumed
         *  (erased per arc) by the reset functions.  Also cleared in
         *  @c resetAll.  When no snapshot exists for an arc, the reset
         *  erases the arc. */
        std::map<std::string, mw::MwArcStats> arc_snapshots_;

        /** Current BSSD pivot satellite per (constellation char + signal-pair key).
         *  Example key: "G_L1C_L2W".  When the pivot changes, the group's SD arcs
         *  are S-transformed onto the new pivot if it was BIA-corrected and
         *  integer-locked at the prior epoch, and erased otherwise, because the SD
         *  ambiguity changes. */
        std::map<std::string, std::string> sd_pivot_sat_;

        /** One-shot deprioritization hints for the next MW pivot pick.
         *  Populated by @c notifyPivotForcedHandover and consumed/cleared
         *  by the next call to @c detectLargeMwResets. */
        std::set<std::string> deprioritized_pivot_sats_;

        /** Cross-satellite MAD σ per (constellation × band-pair) group, keyed by
         *  @c makeGroupKey as "<system_char>_<band1>_<band2>" (e.g. "G_L1_L2";
         *  the tracking attribute is dropped).  Updated at the end of each
         *  @c detectLargeMwResets call from that epoch's pool of per-sat MW
         *  deviations and consumed by @c computeAdaptiveThreshold (through
         *  @c crossSatSigmaFloor) as the warmup floor.  It provides statistical
         *  strength while individual arcs are still warming up (cold start:
         *  10 GPS sats at epoch 2 give a MAD with 9 degrees of freedom against
         *  zero per arc).  Persists across epochs as an EWMA (factor
         *  @c cross_sat_ewma_alpha_, bootstrapped by the first finite MAD);
         *  cleared by @c resetAll.  Lags by one epoch: the current epoch's pool
         *  is complete only after all sats are processed, so the floor uses the
         *  previous epoch's σ.  The lag and the smoothing are acceptable because
         *  the noise environment does not change epoch to epoch; the EWMA keeps
         *  the warmup floor stable instead of following the per-epoch MAD jitter
         *  of a thin cross-section. */
        std::map<std::string, double> cross_sat_sigma_m_;

        /** Per-epoch staging map: collects per-sat MW deviations during
         *  @c processMwSample, finalized into @c cross_sat_sigma_m_ at
         *  the end of @c detectLargeMwResets and then cleared.  Keyed
         *  the same way as @c cross_sat_sigma_m_. */
        std::map<std::string, std::vector<double>> cross_sat_devs_;

        /** Per-epoch set of affirmatively-clean arc keys (see
         *  @c getEpochCleanPairs).  Cleared at the top of every
         *  @c detectLargeMwResets pass and populated in
         *  @c processMwSample for mature arcs that did not flag and whose
         *  sample deviation stayed within a small fraction of λ_WL. */
        std::set<std::string> epoch_clean_pairs_;

        /** Demoted pre-jump level stash per arc key (@ref mw::DemotedArcLevel). */
        std::map<std::string, mw::DemotedArcLevel> demoted_levels_;

        /** Coarser cross-sat σ tiers for the breadth cascade in
         *  @c computeAdaptiveThreshold, derived from the same per-epoch
         *  deviation pool as @c cross_sat_sigma_m_.  @c cross_sat_sigma_cons_
         *  is keyed by single-char constellation ("G") and pools all
         *  band-pairs of that constellation; @c cross_sat_sigma_global_ pools
         *  every constellation except GLONASS (its FDMA inter-channel noise is
         *  the least comparable, so it must not contaminate the estimate the
         *  clean constellations fall back on).  A sparse band-pair group falls
         *  back to its constellation tier, then (non-GLONASS only) to the
         *  global tier, then to the gross-slip gate.  Both persist across
         *  epochs and are cleared by @c resetAll. */
        std::map<std::string, double> cross_sat_sigma_cons_;
        double cross_sat_sigma_global_ = std::numeric_limits<double>::quiet_NaN();

        /** In-house phase-bias estimate per arc key (@ref mw::EstimatedBias),
         *  used only with @c GnssConfig::getInhouseBiasEstimation(). */
        std::map<std::string, mw::EstimatedBias> sat_pair_bias_;

        /**
         * @brief Apply the stored bias for a (sat, signal-pair) to an
         *        incoming raw MW sample, if such a bias exists.
         *
         * @param[in,out] mw_m            MW value (meters), modified in place.
         * @param[in,out] bia_corrected   Becomes true when a stored bias is applied.
         * @param stats_key               "<sat>_<sig1>_<sig2>" key.
         */
        void applyEstimatedBias(double& mw_m, bool& bia_corrected,
                                 const std::string& stats_key) const;

        /**
         * @brief If the arc is eligible, commit (or update) an estimated
         *        bias from its current running mean.
         *
         * @param stats         Arc statistics (modified: mean re-anchored to integer on commit).
         * @param stats_key     "<sat>_<sig1>_<sig2>" key.
         * @param lambda_wl     Wide-lane wavelength (meters).
         * @param epoch_s       Continuous GPS seconds of this epoch.
         * @return true if a bias was committed (or updated) this epoch.
         */
        bool tryCommitOrUpdateBias(mw::MwArcStats& stats,
                                    const std::string& stats_key,
                                    double lambda_wl, double epoch_s);

        /** Effective first-epoch fractional-residual threshold (WL cycles).
         *  Initialized from @c GnssConfig::getMwInitFracThreshold (default
         *  0.4).  Raise it for code-noisy combinations (e.g. L1C-based
         *  MW) where 0.4 is below the single-sample noise floor.  See
         *  the config getter's doc for the noise-vs-threshold reasoning. */
        double init_frac_threshold_;

        /** EWMA smoothing factor for the cross-satellite σ (per tier), from
         *  @c GnssConfig::getMwCrossSatEwmaAlpha (default 0.1, range (0,1]).
         *  The cross-sat MAD is recomputed every epoch from a thin
         *  cross-section (one deviation per non-slipping arc in the group,
         *  ≥ @c kMinCrossSatSamples), which is statistically jumpy
         *  epoch-to-epoch.  Because the MW noise envelope itself varies slowly
         *  (the one-epoch-lag rationale on @c cross_sat_sigma_m_), each tier is
         *  updated as an EWMA, σ_t = α·MAD_epoch + (1−α)·σ_{t−1} with the first
         *  finite MAD as bootstrap, instead of being overwritten with the raw
         *  MAD.  Smaller α = heavier smoothing; α = 1 disables smoothing.
         *  Purely empirical, no stochastic-model input. */
        double cross_sat_ewma_alpha_;

        /** kσ multiplier applied to max(σ_MAD, floor) in
         *  @c computeAdaptiveThreshold, from
         *  @c GnssConfig::getCycleSlipConfig().mw_sigma_multiplier (default 8.0).
         *  Used in both the warmup and mature branches so the gate is continuous
         *  across the handover at @c kMinEpochsForAdaptive.  8σ admits
         *  essentially zero Gaussian false alarms while staying sensitive to
         *  multi-cycle slips above the noise floor.  Raise it (e.g. 12) to widen
         *  the gate when the per-arc TD1 MAD under-estimates the residual spread
         *  on autocorrelated MW noise. */
        double sigma_multiplier_;

        /** Detection-reference selection from
         *  @c GnssConfig::getCycleSlipConfig().mw_detect_reference.  With
         *  @c LocalMedian the slip deviation is measured against the median of
         *  the per-arc @c mw_window ring instead of the lagging Hatch mean, and
         *  the spread is the same window's MAD (×√2 for the TD1-equivalent
         *  multiplier scale).  Deviation and spread are then the same local
         *  quantity, so a smooth MW trend lifts both and cancels instead of
         *  accumulating a lag offset that false-fires the gate.  With
         *  @c TimeDifference there is no level reference: the deviation is the
         *  MW time-difference (order per @c td_order_), gated by the MAD of the
         *  same time-difference history.  That statistic is memoryless and so
         *  immune to trends and to curvature (the soft-quadratic signals that
         *  defeat even the local median), at the cost of √2 (TD1) / √6 (TD2)
         *  noise amplification.  Only the detection statistic is affected: the
         *  Hatch mean, WL lock, drift demotion, and the bias-commit logic are
         *  mode-independent. */
        MwDetectReference detect_reference_;

        /** Differencing order for @c MwDetectReference::TimeDifference, from
         *  @c CycleSlipConfig::mw_td_order.  First: dev = TD1 = mw[k]−mw[k−1],
         *  spread = MAD(@c diff_window), the statistic the legacy mature branch
         *  already maintains, here gating its own quantity.  Second:
         *  dev = TD2 = TD1[k]−TD1[k−1], spread = MAD(@c td2_window).  Ignored
         *  in the level-reference modes. */
        MwTdOrder td_order_;

        /** Effective local-median window length (epochs), from
         *  @c CycleSlipConfig::mw_local_window clamped to
         *  [@c kMinEpochsForAdaptive, @c kMwLocalWindowCap]. */
        size_t local_window_len_;

        /**
         * @brief Compute the adaptive slip-detection threshold for an arc.
         *
         * Two regimes, with a continuous warmup→per-arc handover:
         *
         *  - Mature arc (the caller passes a finite @c mode_sigma_m: the active
         *    statistic's window holds ≥ @c kMinEpochsForAdaptive samples, so the
         *    per-arc MAD is trustworthy): threshold = @c sigma_multiplier_ ×
         *    max(σ_MAD, @c kMinSigmaMeters).  The per-arc per-signal MAD governs.
         *    The cross-sat σ is not a floor here, so one noisy cross-section cannot
         *    override a converged per-arc estimate; only the absolute
         *    @c kMinSigmaMeters anti-collapse floor remains.
         *  - Warmup arc (no trustworthy per-arc MAD yet): threshold =
         *    @c sigma_multiplier_ × max(@c kMinSigmaMeters, cross-sat σ,
         *    mode-specific per-arc spread).  The cross-sat σ (EWMA-smoothed,
         *    resolved by the breadth cascade band-pair → constellation →
         *    global (non-GLONASS), and re-expanded to this arc's elevation)
         *    carries the cold-start epochs until the per-arc MAD takes over.  With
         *    no cross-sat tier populated (a true cold start), the result is
         *    floored at a gross-slip gate of min(λ_WL, 1 m), applied directly
         *    rather than ×k.
         *
         * No stochastic-model-derived σ is consulted at any stage: the gate is
         * entirely data-driven, so a phone and a geodetic receiver each converge to
         * their own MW WL noise level automatically.
         *
         * The caller passes the mature spread of the active detection statistic as
         * @c mode_sigma_m, which keeps this function statistic-agnostic:
         *   - HatchMean: MAD(@c diff_window), the legacy TD1 spread.
         *   - LocalMedian: the raw-MW ring's MAD ×√2 (TD1-equivalent scale).
         *   - TimeDifference/First: MAD(@c diff_window), self-consistent because
         *     the deviation is a TD1 sample.
         *   - TimeDifference/Second: MAD(@c td2_window).
         * When finite it governs the mature branch; when NaN the warmup branch
         * applies, so in every mode the cross-sat floor applies to warmup arcs only.
         *
         * The warmup per-arc spread term is mode-specific.  Level modes use the
         * Welford σ_emp.  TimeDifference modes use the young TD MAD over the partial
         * diff_window / td2_window, because σ_emp is a level variance that a trend
         * inflates by the ramp span (≈ S/√12), which would widen the warmup gate by
         * whole cycles against a noise-scale TD deviation.  In floor position the
         * young MAD cannot collapse the gate (it can lift it, never undercut the
         * other floors), and it makes the warmup→mature handover continuous in the
         * statistic itself.  The cross-sat floor needs no mode handling: its pool
         * already holds active-statistic deviations.
         *
         * @param stats     Current arc statistics for the satellite.
         * @param stats_key Per-arc key used to look up the cross-sat σ
         *                  floor for this (constellation × band-pair)
         *                  group via @c makeGroupKey.  Pass empty string
         *                  to skip the cross-sat lookup.
         * @param elev_deg  Arc elevation (deg).  Re-expands the
         *                  zenith-referenced cross-sat σ floor to this arc's
         *                  elevation via @c mwElevationWeight (low-el arcs get
         *                  a looser floor, high-el a tighter one).
         * @param mode_sigma_m  Mature spread σ (meters) of the active
         *                  detection statistic, per the table above; NaN
         *                  while that statistic's window is immature.
         * @return Adaptive threshold in meters.
         */
        double computeAdaptiveThreshold(const mw::MwArcStats& stats,
                                         const std::string& stats_key,
                                         double elev_deg,
                                         double mode_sigma_m =
                                             std::numeric_limits<double>::quiet_NaN()) const;

        /**
         * @brief Compute the MAD-derived Gaussian-equivalent sigma from an arc's diff window.
         * @param stats  Arc statistics whose diff_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeMadSigma(const mw::MwArcStats& stats);

        /**
         * @brief MAD-derived Gaussian-equivalent σ of the arc's second
         *        time-difference window.
         *
         * Spread source for the @c MwTdOrder::Second gate, with the same
         * @c kMinEpochsForAdaptive maturity floor as the TD1 MAD, so the
         * warmup→mature handover epoch is mode-independent.
         *
         * @param stats  Arc statistics whose td2_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeTd2MadSigma(const mw::MwArcStats& stats);

        /**
         * @brief White-noise scale of the active detection statistic
         *        relative to a raw level MW sample.
         *
         * 1 for the level references (HatchMean and LocalMedian, whose
         * deviations, and hence the cross-sat pool they feed, are
         * level-domain), √2 for TimeDifference/First, √6 for
         * TimeDifference/Second.  Single source of truth for converting
         * between the level noise domain and the active-statistic domain.
         * Its only consumer is the init-frac cross-sat lift, which divides
         * by it (the pool is active-domain, the gated init frac is
         * level-domain).  The warmup gate does not scale σ_emp with it: in
         * the TimeDifference modes that floor term is the young TD MAD,
         * which is already in the active domain.
         *
         * @return Dimensionless σ ratio (≥ 1).
         */
        double activeStatScale() const;

        /**
         * @brief Median (and optionally √2-scaled MAD σ) of the arc's
         *        local raw-MW ring buffer.
         *
         * The median is the local-median detection reference: the level the
         * current sample's deviation is measured against in
         * @c MwDetectReference::LocalMedian mode.  Causal by construction:
         * the ring holds samples through epoch k−1 only (the current sample
         * is pushed after the slip decision), so a slipped current epoch
         * cannot contaminate its own reference.
         *
         * The σ out-param is the same window's MAD × 1.4826 × √2.  The √2
         * keeps the configured @c mw_sigma_multiplier on its legacy scale:
         * the historical spread statistic is the MAD of first
         * time-differences, which for white noise estimates √2·σ_MW, while
         * the window MAD estimates σ_MW directly.  Without the rescale the
         * same multiplier would gate ~1.41× tighter in local mode, and an
         * A/B against the legacy reference would mix the reference change
         * with a threshold change.  σ is reported only once the ring has
         * ≥ @c kMinEpochsForAdaptive accepted samples (the same maturity
         * floor as the TD1 MAD); before that the out-param receives NaN so
         * the warmup floors keep governing the gate.
         *
         * @param stats             Arc whose ring buffer is summarized.
         * @param out_sigma_scaled  Optional; receives the √2-scaled MAD σ
         *                          (meters) or NaN while immature.
         * @return The window median (meters), or NaN if the ring is empty.
         */
        static double computeLocalReference(const mw::MwArcStats& stats,
                                            double* out_sigma_scaled = nullptr);

        /**
         * @brief Append an accepted raw MW sample to the arc's local ring.
         *
         * Mirrors the @c diff_window admission rule: only non-slip epochs
         * are pushed, so a cycle jump never enters the local level estimate.
         * Wraps at the configured @c local_window_len_ (oldest sample
         * overwritten).
         *
         * @param stats  Arc to update.
         * @param mw_m   Accepted MW sample (meters, bias-corrected frame).
         */
        void pushLocalWindow(mw::MwArcStats& stats, double mw_m) const;

        /**
         * @brief Shift every valid sample in the arc's local ring by a
         *        constant (meters).
         *
         * Required wherever an arc survives a reference-frame jump and its
         * @c mean / @c prev_mw are shifted by a constant: the BSSD pivot
         * S-transform (shift by −old-pivot mean) and the in-house bias
         * commit re-anchor (shift by −committed bias).  Without the
         * matching ring shift, the epoch after the frame jump would show a
         * local deviation equal to the full frame offset, a false slip on
         * every surviving arc.
         *
         * @param stats    Arc whose ring is shifted.
         * @param delta_m  Additive shift applied to each sample (meters).
         */
        static void shiftLocalWindow(mw::MwArcStats& stats, double delta_m);

        /**
         * @brief Compute MAD-derived Gaussian-equivalent σ from an
         *        arbitrary vector of deviations (in meters).
         *
         * @param devs  Vector of deviations.  Must have at least
         *              @c kMinCrossSatSamples entries to return a finite
         *              value.
         * @return σ in meters, or NaN if the pool is too small.
         */
        static double computeMadOfDevs(std::vector<double> devs);

        /**
         * @brief Derive the cross-sat group key from a per-arc stats key.
         *
         * Reduces the satellite to its constellation prefix and each signal to
         * its 2-char frequency band (attribute dropped), so all attribute
         * combinations on the same frequency pair pool into one statistic:
         *   "G01_L1C_L2W" → "G_L1_L2"
         *   "G01_L1L_L2L" → "G_L1_L2"  (same group)
         * Distinct frequency pairs (L1/L2 vs L1/L5) remain separate.  This
         * gives sparse attribute combinations enough samples to clear
         * @c kMinCrossSatSamples instead of collapsing to the σ floor.
         *
         * @param stats_key  Per-arc key from @c mw_stats_.
         * @return Group key, or empty string on malformed input.
         */
        static std::string makeGroupKey(const std::string& stats_key);

        /**
         * @brief Zenith-referenced elevation weight of the MW noise, w(el) = 1/sin(el),
         *        with the elevation clamped at 10 degrees.
         *
         * A deviation enters the cross-satellite pool divided by it (zenith-normalized), and
         * the pooled zenith σ leaves @c crossSatSigmaFloor multiplied by it (re-expanded to
         * the arc's elevation).  The rationale is at the definition
         * (MwSlipDetectorStats.cpp).
         *
         * @param elev_deg  Arc elevation (deg); a non-finite value counts as 10.
         * @return The weight (≥ 1, at most 1/sin 10° ≈ 5.76).
         */
        static double mwElevationWeight(double elev_deg);

        /**
         * @brief Finalize cross-sat σ from this epoch's staging pool.
         *
         * Computes MAD × 1.4826 at three breadth tiers from the same staging
         * pool: per (constellation × band-pair) → @c cross_sat_sigma_m_, per
         * constellation → @c cross_sat_sigma_cons_, and global (all
         * constellations except GLONASS) → @c cross_sat_sigma_global_.  Each
         * tier updates only when it clears @c kMinCrossSatSamples; otherwise
         * its previous σ persists (or no entry, if cold-start).  Then clears
         * @c cross_sat_devs_ for the next epoch.
         *
         * Each tier is EWMA-blended (factor @c cross_sat_ewma_alpha_) onto its
         * previous value rather than overwritten, so the slowly-varying noise
         * envelope is tracked without the per-epoch MAD jitter that a thin
         * cross-section produces; the first finite MAD bootstraps the average.
         *
         * Called at the end of @c detectLargeMwResets.
         */
        void finalizeCrossSatSigmas();

        /**
         * @brief Resolve the cross-satellite σ floor for an arc (meters).
         *
         * Looks up the (EWMA-smoothed) cross-sat σ via the breadth cascade
         * band-pair → constellation → global(non-GLONASS), then re-expands the
         * zenith-referenced value to this arc's elevation via
         * @c mwElevationWeight.  Shared by @c computeAdaptiveThreshold (warmup
         * floor) and the @c mwwl_trace.csv writer (the @c cross_sat_floor_m
         * diagnostic column), so the trace shows the floor even on mature arcs,
         * where it does not gate.
         *
         * @param stats_key Per-arc key; pooled by (constellation × band-pair).
         * @param elev_deg  Arc elevation (deg) for the 1/sin re-expansion.
         * @return Cross-sat σ floor in meters, or NaN when no tier is populated
         *         (true cold start) or @p stats_key is empty.
         */
        double crossSatSigmaFloor(const std::string& stats_key,
                                  double elev_deg) const;

        /** Output stream for MWWL trace CSV file (mutable for const access). */
        mutable std::ofstream mwwl_trace_;

        /** Current epoch time for tracing (set at start of detectLargeMwResets). */
        mutable GnssTime current_epoch_;

        /**
         * @brief Matched IF phase/code label pair.
         */
        struct IfPair {
            std::string phase_key;  ///< e.g. "L1W_L5Q"
            std::string code_key;   ///< e.g. "C1W_C5Q"
        };

        /**
         * @brief Collect all matched ionosphere-free phase/code pairs.
         *
         * Returns every IF phase label that has a corresponding code label
         * using the same pair of frequency bands.
         *
         * @param obs Observation to inspect.
         * @return Vector of matched pairs (may be empty).
         */
        std::vector<IfPair> collectIonoFreePairs(const PppObservation& obs) const;

        /**
         * @brief Split an IF label of the form "L1C_L2W" into its two component signals.
         *
         * Example: input "L1C_L2W" -> outputs ("L1C","L2W").
         *
         * @param label Input IF label.
         * @param[out] sig1 First signal token.
         * @param[out] sig2 Second signal token.
         * @return true when the label could be split into exactly two tokens.
         * @return false otherwise.
         */
        bool splitIfLabel(const std::string& label, std::string& sig1, std::string& sig2) const;

        /**
         * @brief Compute the effective wide-lane wavelength (meters) for a given observation and signal pair.
         *
         * Uses PppUtils to obtain nominal frequencies/wavelengths for the signals
         * in @p sig1 and @p sig2, and computes the wide-lane wavelength in meters.
         *
         * @param obs Observation whose system is used to resolve frequencies.
         * @param sig1 First signal identifier (e.g. "L1C").
         * @param sig2 Second signal identifier (e.g. "L2W").
         * @return double Wide-lane wavelength in meters, or 0.0 on error.
         */
        double computeWideLaneWavelengthMeters(const PppObservation& obs, const std::string& sig1, const std::string& sig2) const;

        /**
         * @brief Compute the Melbourne-Wübbena Wide-Lane (MWWL) observable in meters.
         *
         * Computes the full MW combination:
         *   MW = (f1·L1 - f2·L2)/(f1-f2) - (f1·P1 + f2·P2)/(f1+f2)
         *
         * where L1, L2 are phase observables (meters) and P1, P2 are code
         * observables (meters). The result is geometry-free and ionosphere-free,
         * proportional to the wide-lane ambiguity plus noise.
         *
         * Code signals are provided explicitly (from the iono-free code
         * combination) rather than derived from the phase labels, to ensure
         * matched tracking modes on each frequency and avoid inter-signal
         * code biases that would introduce periodic trends.
         *
         * @param obs Observation containing raw phase/code values and metadata.
         * @param sig1 First phase signal identifier (e.g. "L1C").
         * @param sig2 Second phase signal identifier (e.g. "L2W").
         * @param code1 First code signal identifier (e.g. "C1W").
         * @param code2 Second code signal identifier (e.g. "C2W").
         * @return double MW value in meters. Returns NaN on invalid input.
         */
        double computeMwMeters(const PppObservation& obs,
            const std::string& sig1,
            const std::string& sig2,
            const std::string& code1,
            const std::string& code2) const;

        /**
         * @brief Compute the satellite MW bias from BIA phase and code entries.
         *
         * Computes:
         *   b_MW = (f1·b_L1 - f2·b_L2)/(f1-f2) - (f1·b_C1 + f2·b_C2)/(f1+f2)
         *
         * where b_L1/b_L2 and b_C1/b_C2 are phase and code biases (meters)
         * from the BIA product.  The result is in meters and can be subtracted
         * from the raw MW to produce a near-integer WL observable.
         *
         * @param obs   Observation (for satellite, system, time, frequency lookup).
         * @param sig1  First phase signal (e.g. "L1W").
         * @param sig2  Second phase signal (e.g. "L2W").
         * @param code1 First code signal (e.g. "C1W").
         * @param code2 Second code signal (e.g. "C2W").
         * @param[out] bias_m  Computed satellite MW bias in meters.
         * @return True if all four BIA entries were found and the bias is finite.
         */
        bool computeSatMwBias(const PppObservation& obs,
            const std::string& sig1, const std::string& sig2,
            const std::string& code1, const std::string& code2,
            double& bias_m) const;

        /**
         * @brief A dual-frequency pair extracted from uncombined observations.
         *
         * Groups the phase and code signal identifiers for two frequencies
         * so that an MW combination can be formed.
         */
        struct UncombinedPair {
            std::string phase1;  /**< Phase signal on freq 1 (e.g. "L1C"). */
            std::string phase2;  /**< Phase signal on freq 2 (e.g. "L2W"). */
            std::string code1;   /**< Code  signal on freq 1 (e.g. "C1W"). */
            std::string code2;   /**< Code  signal on freq 2 (e.g. "C2W"). */
        };

        /**
         * @brief Collect viable dual-frequency MW pairs from observations.
         *
         * Groups individual code and phase signals by RINEX band number (the
         * digit after the C/L prefix, e.g. "1" from "L1C"), then forms
         * all dual-frequency pairs that have both a code and a phase
         * observable on each band.
         *
         * Source and within-band multiplicity are controlled by the two
         * independent MW knobs @c GnssConfig::getMwFrequencyMode and
         * @c GnssConfig::getMwChannelMode (each resolves through Mirror
         * to the KF's setting):
         *  - mw_frequency_mode = Mirror (default): uses obs.observed
         *    (the KF's active bands), keeping the MW detector aligned
         *    with the Kalman filter.
         *  - mw_frequency_mode = Dual / Triple / All: uses obs.raw_observed
         *    for cross-frequency cross-consistency.
         *  - mw_channel_mode controls whether one canonical tracking
         *    attribute per band (Single) or every tracked channel (All)
         *    is kept.
         *
         * @param obs Observation to inspect.
         * @return std::vector<UncombinedPair> Viable MW pairs (may be empty).
         */
        std::vector<UncombinedPair> collectUncombinedPairs(const PppObservation& obs) const;

        /**
          * @brief Check whether the observation reports any loss-of-phase-lock indicators.
          *
          * Returns true when any of the observables in @p obs have non-zero LLI
          * flags indicating potential cycle slips or loss of lock.
          *
          * @param obs Observation to inspect.
          * @return true If any phase loss-of-lock indicators are present.
          * @return false Otherwise.
         */
        bool hasAnyPhaseLockLoss(const PppObservation& obs) const;

        /**
         * @brief Write a single MW observation record to the trace CSV.
         *
         * Called internally during detectLargeMwResets() when tracing is enabled.
         *
         * @param obs Observation being processed.
         * @param sig1 First signal identifier (e.g. "L1C").
         * @param sig2 Second signal identifier (e.g. "L2W").
         * @param mw_m Current MW value in meters.
         * @param mw_mean Running mean of MW values in meters.
         * @param dev_from_mean Deviation of current MW from running mean (meters).
         * @param mw_sigma MAD-derived sigma estimate (meters, NaN if unavailable).
         * @param arc_count Number of epochs in the current arc.
         * @param lambda_wl Wide-lane wavelength (meters).
         * @param thresh_m Threshold in meters.
         * @param is_slip True if this observation triggered a slip detection.
         * @param is_lli True if loss-of-lock was detected.
         * @param mw_pivot SD pivot satellite for this row's (constellation,
         *                 signal-pair) group; empty for undifferenced arcs.
         * @param local_ref_m Local-median reference level (meters; NaN when
         *                 the ring is empty).  Emitted in every reference
         *                 mode, so a run in another mode still records what
         *                 the local reference would have been (offline A/B).
         * @param local_sigma_m √2-scaled local-window MAD σ (meters; NaN
         *                 while the ring is immature).
         * @param detect_mode Statistic that governed this row's slip decision:
         *                 0 = Hatch-mean level deviation, 1 = local-median level
         *                 deviation (also the level-disagreement backstop rows,
         *                 whose quantities are synthesized), 2 = first
         *                 time-difference, 3 = second time-difference.  Rows that
         *                 bypass the dev test (arc-init, LLI) carry 0.
         * @param active_dev_m The deviation the detector actually gated this row on
         *                 (meters): dev_from_mean in mode 0, the local/TD deviation
         *                 in modes 1–3, the synthesized level gap on backstop rows;
         *                 NaN where the dev test is bypassed.  Makes every row
         *                 self-describing: |active_dev|/threshold > 1 iff is_slip,
         *                 in every mode.
        */
        void writeTraceRecord(const PppObservation& obs,
                            const std::string& sig1,
                            const std::string& sig2,
                            double mw_m,
                            double mw_mean,
                            double dev_from_mean,
                            double mw_sigma,
                            size_t arc_count,
                            double lambda_wl,
                            double thresh_m,
                            double cross_sat_floor_m,
                            bool is_slip,
                            bool is_lli,
                            bool bia_corrected = false,
                            double sat_mw_bias_m = 0.0,
                            int rounded_wl = 0,
                            bool wl_locked = false,
                            const std::string& mw_pivot = {},
                            double local_ref_m =
                                std::numeric_limits<double>::quiet_NaN(),
                            double local_sigma_m =
                                std::numeric_limits<double>::quiet_NaN(),
                            int detect_mode = 0,
                            double active_dev_m =
                                std::numeric_limits<double>::quiet_NaN()) const;

        // ======================================================================
        // The two drivers' steps
        //
        // detectLargeMwResets and processMwSample are ordered drivers over the
        // private steps below; the working values the steps share are declared
        // here and defined in slip/MwDetectorWork.hpp, which lists the steps.
        // ======================================================================

        /**
         * @brief The lane a sample belongs to: one satellite and one signal pair.
         *
         * The arc key is "<sat>_<sig1>_<sig2>"; on the combined path it is written
         * "<sat>_<IF label>", the same string, since the IF label is "<sig1>_<sig2>".
         */
        struct MwLane {
            std::string satellite;   ///< Satellite identifier (e.g. "G01").
            std::string sig1;        ///< First phase signal (higher frequency, e.g. "L1C").
            std::string sig2;        ///< Second phase signal (lower frequency, e.g. "L2W").
            std::string code1;       ///< RINEX code observable the MW combination consumed on
                                     ///< @c sig1, recorded so a consumer can name the satellite
                                     ///< code-bias column it carries (@ref MwArcZdMean::code1).
                                     ///< Bookkeeping only: the MW value arrives pre-computed.
            std::string code2;       ///< RINEX code observable on @c sig2 (see @c code1).
            std::string if_label;    ///< Combined (ionosphere-free) path: the IF phase label
                                     ///< ("L1C_L2W"), whose one ambiguity a flag resets.  Empty
                                     ///< on the uncombined path, where a flag resets both
                                     ///< signals' ambiguities.
            std::string stats_key;   ///< The arc key into @c mw_stats_ (see above).
        };

        /**
         * @brief One MW value of a lane, as the caller formed it.
         */
        struct MwSample {
            double mw_m{ 0.0 };            ///< MW value (meters), already bias-corrected by the
                                           ///< caller (satellite BIA; single-differenced
                                           ///< against the group pivot on a BIA arc).
            double lambda_wl{ 0.0 };       ///< Wide-lane wavelength (meters).
            bool   bia_corrected{ false }; ///< True if both the satellite (BIA) and the receiver
                                           ///< (BSSD) biases are removed: the value is near-integer
                                           ///< in WL cycles and eligible for rounding.
            double sat_mw_bias{ 0.0 };     ///< Satellite MW bias that was applied (meters, for
                                           ///< the trace).
        };

        /**
         * @brief The single-difference frame of a sample: the MW pivot of its
         *        (constellation, signal-pair) group and the pivot's own slip this pass.
         *
         * The default is the undifferenced frame: no pivot, nothing moved.
         */
        struct MwSdFrame {
            std::string pivot_sat;          ///< SD pivot satellite, recorded in the trace's
                                            ///< @c mw_pivot column; empty for an undifferenced
                                            ///< (no-BIA) arc.
            int  pivot_zd_slip{ 0 };        ///< The pivot's own ZD slip this pass (cycles; 0
                                            ///< when it is clean or the arc is undifferenced).
                                            ///< Consumed only by the locked-arc TD confirmation,
                                            ///< where non-zero marks the SD frame as moved: a new
                                            ///< pending flag is not deferred, an existing one is
                                            ///< not classified.
            bool pivot_zd_known{ true };    ///< Whether the pivot's ZD slip integer is known;
                                            ///< false likewise marks the frame moved.
        };

        /**
         * @brief What @c processMwSample reports for a flagged sample.
         *
         * The member initializers are the values a path that does not set a field reports.
         * A path with no meaningful strength leaves @c evidence_ratio NaN instead of
         * inventing one, so @c std::isfinite on it tells whether the path published one.
         * The integer and the float are in the sample's frame (single-differenced on a BIA
         * arc): the caller re-references them to the satellite's own ZD slip.
         */
        struct MwVerdict {
            int    slip_cycles{ 0 };       ///< Integer WL slip magnitude (valid when @c wl_known).
            bool   wl_known{ false };      ///< True when @c slip_cycles is reliable (only the
                                           ///< unlocked-arc path publishes one).
            double slip_frac{ 0.5 };       ///< |x - round(x)| of the float value before rounding
                                           ///< (0 = exact, up to 0.5 = worst).
            double slip_wl_float{ std::numeric_limits<double>::quiet_NaN() };
                                           ///< Float WL-step estimate (cycles) where the path has
                                           ///< one (the unlocked-arc jump, the TD-confirmation
                                           ///< level gap, the drift, the level-disagreement gap);
                                           ///< used to select candidates, never as an integer.
            double evidence_ratio{ std::numeric_limits<double>::quiet_NaN() };
                                           ///< |dev|/threshold at the firing gate.
            int    arc_epochs{ 0 };        ///< The arc's sample count before the erase (0 on the
                                           ///< init-suspicious path).
        };

        struct SatMwEntry;    ///< One satellite's BIA-corrected sample of an SD group (slip/MwDetectorWork.hpp).
        struct MwSampleWork;  ///< The working value of processMwSample's steps (slip/MwDetectorWork.hpp).
        struct MwEpochWork;   ///< The working value of detectLargeMwResets' steps (slip/MwDetectorWork.hpp).

        /**
         * @brief Process one MW sample of a lane: update the arc's statistics or flag a slip.
         *
         * The per-sample driver: twelve steps over one @c MwSampleWork, ending at the first
         * step that flags a slip.  For BIA+BSSD-corrected arcs the WL integer locks once the
         * statistical lock criterion passes (n ≥ 10).  A known integer slip ΔN_WL is reported
         * only by the unlocked-arc path, when the jump deviation is near-integer and, in the
         * TimeDifference modes, resolvable from a single sample.  Every locked-arc detection
         * (drift demotion, TD confirmation) reports an unknown integer, so the caller resets.
         *
         * @param obs     Observation (the epoch time, the elevation, tracing metadata).
         * @param lane    The satellite and signal pair (@ref MwLane).
         * @param sample  The MW value and its frame flags (@ref MwSample).
         * @param frame   The single-difference frame (@ref MwSdFrame); default undifferenced.
         * @return The verdict when a slip was flagged (the caller generates reset keys);
         *         empty otherwise.
         */
        std::optional<MwVerdict> processMwSample(const PppObservation& obs, const MwLane& lane,
                                                 const MwSample& sample, const MwSdFrame& frame);

        // --- processMwSample's steps (MwSlipDetectorSample.cpp) --------------------------

        /**
         * @brief S1: apply a stored in-house bias, reset the arc on a data gap, stamp its code
         *        channels, snapshot it for rollback, and stamp the per-epoch pair status.
         * @param w  The sample's working value; sets @c stats, @c epoch_s, @c lli_shadowed.
         */
        void stampArc_(MwSampleWork& w);

        /**
         * @brief S2: open a fresh arc with its first sample, and flag a BIA-corrected first
         *        sample whose fractional offset says a slip happened during the gap.
         * @param w  The sample's working value (an arc with no samples).
         * @return True if the first sample is flagged (the verdict is set).
         */
        bool initArc_(MwSampleWork& w);

        /**
         * @brief S3: form this sample's TD1 and resolve a pending locked-arc TD confirmation:
         *        a held level is a step (emit an unknown-integer slip and erase the arc), a
         *        returned level an outlier (keep the lock, excise it from the TD chain).
         * @param w  The sample's working value; sets @c mw_diff.
         * @return True if a confirmed step was emitted.
         */
        bool resolveTdConfirm_(MwSampleWork& w);

        /**
         * @brief S4: lock the WL integer of a BIA-corrected arc once the statistical lock
         *        criterion passes.
         * @param w  The sample's working value.
         */
        void tryLock_(MwSampleWork& w) const;

        /**
         * @brief S5: commit (or update) the in-house bias of an unbiased arc and move the
         *        sample into the corrected frame (processing.inhouse_bias_estimation only).
         * @param w  The sample's working value.
         */
        void tryInhouseCommit_(MwSampleWork& w);

        /**
         * @brief S6: the deviation and the mature spread the gate tests, per the detection
         *        reference (Hatch mean, local median, first or second time-difference).
         * @param w  The sample's working value; sets the statistic's members.
         */
        void selectStatistic_(MwSampleWork& w) const;

        /**
         * @brief S7: the adaptive threshold and the flag verdict in the active and the Hatch
         *        frame, the pair-status record and the MWDEC decision line.
         * @param w  The sample's working value; sets @c thresh, @c sigma, @c is_slip,
         *           @c is_slip_level.
         */
        void gate_(MwSampleWork& w);

        /**
         * @brief S8: the score cache, the affirmative-clean set, the cross-satellite pool
         *        and the trace row.
         * @param w  The sample's working value.
         */
        void recordEpochEvidence_(MwSampleWork& w);

        /**
         * @brief S9: a flagged sample.  A locked arc absorbs it, defers it to a TD
         *        confirmation, or (under a pivot slip) emits at once; an unlocked arc emits
         *        it, with a known integer when the jump rounds cleanly, and is erased.
         * @param w  The sample's working value.
         * @return True if a slip was emitted.
         */
        bool onFlag_(MwSampleWork& w);

        /**
         * @brief S10: fold the sample into the Welford/Hatch statistics and the windows.
         * @param w  The sample's working value.
         */
        void foldSample_(MwSampleWork& w) const;

        /**
         * @brief S11: demote a locked arc whose live mean drifted from its integer, and emit
         *        an unknown-integer slip.
         * @param w  The sample's working value.
         * @return True if the lock was demoted (a slip emitted).
         */
        bool checkDriftDemotion_(MwSampleWork& w);

        /**
         * @brief S12: restart an unlocked arc whose Hatch mean and local median disagree by
         *        more than the drift bound (an absorbed step), emitting an unknown-integer slip.
         * @param w  The sample's working value.
         * @return True if a slip was emitted.
         */
        bool checkLevelBackstop_(MwSampleWork& w);

        // --- detectLargeMwResets' steps (MwSlipDetectorEpoch.cpp) -------------------------

        /**
         * @brief E0: the per-epoch resets (snapshots, clean set, pair status), the LLI
         *        rotation, the epoch time, and the aging of demoted-level stashes.
         * @param w             The epoch's working value.
         * @param observations  The epoch's observations.
         */
        void beginEpoch_(MwEpochWork& w, const std::vector<PppObservation>& observations);

        /**
         * @brief E1: one satellite of pass 1.  Fold the phase bridge, then take the path:
         *        the combined path (IF labels present) or the uncombined path, each with its
         *        own LLI policy.
         * @param w    The epoch's working value.
         * @param obs  The satellite's observation (above the elevation mask).
         */
        void collectLaneSamples_(MwEpochWork& w, const PppObservation& obs);

        /**
         * @brief The combined path's LLI policy: schedule each IF ambiguity's reset, write the
         *        LLI trace rows, record the flagged bands, and erase the satellite's arcs.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param if_pairs  Its ionosphere-free phase/code label pairs.
         */
        void eraseArcsOnLli_(MwEpochWork& w, const PppObservation& obs,
                             const std::vector<IfPair>& if_pairs);

        /**
         * @brief The uncombined path's LLI policy: schedule every phase ambiguity's reset and
         *        record the flagged bands for the next epoch's LLI shadow, keeping the arcs.
         * @param w    The epoch's working value.
         * @param obs  The satellite's observation.
         */
        void preserveArcsOnLli_(MwEpochWork& w, const PppObservation& obs);

        /**
         * @brief The combined path's IF lanes: form each lane's MW; a BIA-corrected lane joins
         *        its SD group, another is processed undifferenced at once.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param if_pairs  Its ionosphere-free phase/code label pairs.
         * @return The band pairs ("1_2") the IF lanes cover.
         */
        std::set<std::string> collectCombinedLanes_(MwEpochWork& w, const PppObservation& obs,
                                                    const std::vector<IfPair>& if_pairs);

        /**
         * @brief The combined path's supplementary lanes: uncombined band pairs the IF labels
         *        do not cover, routed as in @c collectCombinedLanes_.
         * @param w              The epoch's working value.
         * @param obs            The satellite's observation.
         * @param if_band_pairs  The band pairs the IF lanes cover.
         */
        void collectSupplementaryLanes_(MwEpochWork& w, const PppObservation& obs,
                                        const std::set<std::string>& if_band_pairs);

        /**
         * @brief The uncombined path's lanes: one per band pair, routed as in
         *        @c collectCombinedLanes_.
         * @param w         The epoch's working value.
         * @param obs       The satellite's observation.
         * @param uc_pairs  Its viable MW pairs.
         */
        void collectUncombinedLanes_(MwEpochWork& w, const PppObservation& obs,
                                     const std::vector<UncombinedPair>& uc_pairs);

        /**
         * @brief E2a: elect an SD group's MW pivot: the highest elevation, skipping the
         *        deprioritized satellites unless every member is one.
         * @param sat_entries  The group's members, by satellite.
         * @return The pivot satellite, or empty if none can be elected.
         */
        std::string electPivot_(const std::map<std::string, SatMwEntry>& sat_entries) const;

        /**
         * @brief E2b: when the group's pivot changes, S-transform its locked SD arcs onto the
         *        new pivot (or erase them), drop the old pivot's ZD arc, and re-reference the
         *        stored in-house biases.
         * @param w            The epoch's working value.
         * @param group_key    The group ("<cons>_<sig1>_<sig2>").
         * @param sat_entries  The group's members.
         * @param best_pivot   The elected pivot.
         */
        void reReferenceGroup_(MwEpochWork& w, const std::string& group_key,
                               const std::map<std::string, SatMwEntry>& sat_entries,
                               const std::string& best_pivot);

        /**
         * @brief E2c: the pivot's own undifferenced sample; its ZD slip anchors the group's
         *        frame.
         * @param w            The epoch's working value.
         * @param sat_entries  The group's members.
         * @param best_pivot   The pivot.
         * @return The group's SD frame: the pivot and its ZD slip this pass.
         */
        MwSdFrame processPivot_(MwEpochWork& w,
                                const std::map<std::string, SatMwEntry>& sat_entries,
                                const std::string& best_pivot);

        /**
         * @brief E2d: the non-pivot members, single-differenced against the pivot; a flag's
         *        SD jump is re-referenced to the satellite's own ZD slip.
         * @param w            The epoch's working value.
         * @param sat_entries  The group's members.
         * @param best_pivot   The pivot.
         * @param frame        The group's SD frame (@c processPivot_).
         */
        void processNonPivots_(MwEpochWork& w,
                               const std::map<std::string, SatMwEntry>& sat_entries,
                               const std::string& best_pivot, const MwSdFrame& frame);

        /**
         * @brief E3: consume the pivot hints, finalize the cross-satellite σ tiers, flush the
         *        trace.
         * @param w  The epoch's working value.
         * @return The epoch's results.
         */
        std::vector<MwSlipResult> endEpoch_(MwEpochWork& w);

        /**
         * @brief Append the results of one flagged lane: one per ambiguity it resets (the IF
         *        ambiguity of a combined lane; the two signals' of an uncombined lane).
         * @param results        The epoch's results.
         * @param lane           The flagged lane.
         * @param verdict        Its verdict (re-referenced to ZD by the caller where needed).
         * @param lambda_wl      Its wide-lane wavelength (meters).
         * @param bia_corrected  The lane class the results report (see
         *                       @ref MwSlipResult::bia_corrected).
         */
        static void emitLaneSlip_(std::vector<MwSlipResult>& results, const MwLane& lane,
                                  const MwVerdict& verdict, double lambda_wl,
                                  bool bia_corrected);

        /// Unit-test seam (tests/test_mw_detector_steps.cpp): reaches the steps above.
        friend struct MwDetectorStepsTestAccess;


        // ======================================================================
        // Network-only surface, owned with card NOM
        //
        // The per-station detectors of the network producer call these; the user
        // drivers (post-processing, real time, baseline) and the closed-loop
        // consumer never do.  The phase bridge, the
        // zero-differenced and cold arc reads (WL-1, WL-3, UWL-1), the per-epoch
        // pair status with its LLI shadow (SPEC_event_harness §14) and the
        // station label of the debug lines.  The bridge stays off and the label
        // empty unless the network arms them; the pair status and the LLI shadow
        // are written on every path and read only by the network.  None of them
        // changes a detection.
        // ======================================================================
    public:
        /**
         * @brief Name this detector instance for its debug-level decision lines
         *        (SPEC_event_harness.md §14.10).
         *
         * The network runs one detector per station, and all of them share the
         * `mw_slip_detector` logger, whose records are keyed only by
         * `<sat>_<sig1>_<sig2>`; without a label, two stations' lines for the
         * same arc cannot be told apart.  The label is stamped into the MWDEC /
         * MWSKIP lines.  Empty by default; affects nothing but log text.
         *
         * @param label Station name (or any short instance id).
         */
        void setStationLabel(const std::string& label) { station_label_ = label; }

        /**
         * @brief Per-epoch record of every arc the detector processed a sample for.
         *
         * Same key shape and lifetime as @c getEpochCleanPairs
         * ("&lt;sat&gt;_&lt;sig1&gt;_&lt;sig2&gt;", cleared and rebuilt by every
         * @c detectLargeMwResets call), but it answers a different question.
         *
         * @c getEpochCleanPairs answers "is this arc clean enough to assert
         * ΔN_WL = 0 for TurboEdit's GF-only split": a conjunction of a 0.3-cycle
         * deviation bound and a σ condition (3σ ≤ 0.7·λ_WL) that a narrow lane on
         * a noisy bed never satisfies, however quiet it is.  On the 60-station
         * sim, the MW scatter σ_MAD p50 is 0.414 m on L1C+L2W against that pair's
         * 0.201 m bar, so the narrow lanes are never affirmed and only the
         * extra-wide lanes (λ_WL 5.9–9.8 m) appear there.
         *
         * This map answers the weaker, detection-grade question a slip-response
         * consumer needs: "did the detector look at this pair this epoch, on an
         * arc old enough to have flagged, and not flag it?"  The network
         * producer's common-band scope (SPEC_event_harness.md §14) is built on
         * it and reads @ref EpochPairStatus::thresh_m, so a silence too loose to
         * exclude the slip size in question can be counted instead of trusted.
         *
         * @return Map from processed arc key to that sample's detection record.
         */
        const std::map<std::string, EpochPairStatus>& getEpochPairStatus() const { return epoch_pair_status_; }

        /**
         * @brief Read a (satellite, signal-pair) MW arc as a ZD-consistent Welford
         *        summary: the per-arc code-coherence accumulator that the wide-lane
         *        layer double-differences over the Card-4 chord basis.
         *
         * The network per-station detector stores non-pivot arcs single-differenced
         * against a per-(constellation,signal-pair) MW pivot (mean = MW_bia(sat) -
         * MW_bia(pivot)) and the pivot arc undifferenced (mean = MW_bia(pivot)).  For a
         * non-pivot (bia_corrected) arc this re-adds the group pivot's mean to recover
         * the undifferenced value MW_bia(sat); a pivot arc or a no-BIA-fallback arc
         * (both stored undifferenced) is returned as-is.  When the arc is
         * single-differenced but its group pivot arc is absent (cannot reconstruct) the
         * result is @c valid=false and the caller skips the cycle.
         *
         * Maturity is not gated here: @c count is returned so the caller applies its
         * own @c min_arc_epochs threshold.
         *
         * @param sat  Satellite identifier (e.g. "G05").
         * @param sig1 First (higher-frequency / lower-band) phase signal, e.g. "L1C".
         * @param sig2 Second (lower-frequency / higher-band) phase signal, e.g. "L2W".
         * @return The ZD-consistent arc summary; @c valid=false if no usable arc exists.
         */
        MwArcZdMean arcZdMean(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2) const;

    private:
        // --- The per-epoch pair status and its LLI shadow (SPEC_event_harness §14) ---

        /** Per-epoch detection record of every arc processed this epoch (see
         *  @c getEpochPairStatus).  Written twice per sample in
         *  @c processMwSample: once right after the gap check, so an arc that
         *  leaves through an early return is still recorded, and again once the
         *  adaptive threshold and flag verdict exist.  Cleared alongside
         *  @ref epoch_clean_pairs_. */
        std::map<std::string, EpochPairStatus> epoch_pair_status_;

        /** LLI shadow memory (SPEC_event_harness.md §14.11): satellite → the phase
         *  bands that carried a receiver loss-of-lock bit, for the previous epoch
         *  (@ref prev_lli_bands_) and the one being processed (@ref cur_lli_bands_).
         *  Rotated once at the top of @c detectLargeMwResets, so a sample stamped
         *  during this epoch reads the previous epoch's flags.  The LLI epoch
         *  itself is withheld (the detector skips the satellite with `continue`),
         *  and the first sample after it is the one whose silence carries no
         *  information.  The shadow lasts one epoch: by the second sample the arc
         *  has seen a post-gap value and its silence is informative again.  The
         *  memory is detector-local, so its scope is the per-station instance and
         *  nothing leaks across stations. */
        std::map<std::string, std::set<std::string>> prev_lli_bands_;
        std::map<std::string, std::set<std::string>> cur_lli_bands_;

        /** Instance label for the MWDEC / MWSKIP debug lines; see
         *  @ref setStationLabel.  Log text only. */
        std::string station_label_;
        };

} // namespace GnssProcessor
