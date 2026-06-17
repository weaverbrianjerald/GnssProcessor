#pragma once

#pragma once

/**
 * @file MwSlipDetector.hpp
 * @brief Detects large wide-lane (MW) resets/slips using ionosphere-free combinations.
 *
 * The MwSlipDetector identifies large changes in MW-like observables between
 * epochs which typically indicate cycle slips or ambiguity resets. The detector
 * produces a list of ambiguity keys (e.g. "G01_L1C_L2W") that should be reset
 * when a large MW discontinuity is detected.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "slip/ISlipDetector.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Result of MW slip detection for a single ambiguity.
     *
     * The MW observable measures N_WL = N_1 - N_2.  A WL slip tells us
     * the *difference* changed but cannot distinguish which signal slipped.
     * When slip_wl_known is true, slip_wl_cycles contains an integer
     * estimate of the WL jump, and slip_frac records how far from integer
     * the raw value was before rounding (0 = exact, up to 0.5 = worst).
     *
     * @note slip_wl_cycles is always the satellite's OWN undifferenced
     *       (zero-differenced) WL slip.  Bias-corrected arcs are detected in a
     *       single-differenced frame against the detector's internal MW pivot,
     *       but that pivot's own slip is added back before reporting, so
     *       consumers may difference two satellites' slips directly without
     *       knowing (or sharing) the MW pivot.  When the conversion can't be
     *       made (MW pivot slipped with an unresolved integer) slip_wl_known is
     *       set false so the consumer falls back to a reset.
     */
    struct MwSlipResult {
        std::string amb_key;           ///< Ambiguity key (e.g. "G01_L1C").
        int    slip_wl_cycles{ 0 };    ///< Satellite's OWN (zero-differenced) WL slip dN_WL, cycles (0 if unknown).
        bool   slip_wl_known{ false }; ///< True if slip_wl_cycles is a usable integer.
        double lambda_wl{ 0.0 };       ///< Wide-lane wavelength (meters) for this signal pair.
        std::string sig1;              ///< First signal (higher freq, e.g. "L1C").
        std::string sig2;              ///< Second signal (lower freq, e.g. "L2W").
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        double slip_frac{ 0.0 };       ///< Fractional residual |x - round(x)| before rounding
                                       ///< (0 = exact integer, 0.5 = worst).  Clean pairs and
                                       ///< BIA-locked pairs have ~0; best-effort pairs may be large.
        /// FLOAT estimate of the WL slip (cycles, same zero-differenced frame
        /// as @c slip_wl_cycles), populated where the emission path has a
        /// meaningful step estimate even when @c slip_wl_known is false —
        /// e.g. the locked-arc TD-confirmation level gap, whose integer is
        /// withheld by contract (the deferred path must never publish one)
        /// but whose magnitude is sound.  NaN when no estimate exists or the
        /// SD→ZD re-referencing could not be applied (pivot slip unresolved).
        /// Consumers must treat it as a CANDIDATE SELECTOR only (e.g.
        /// TurboEditResolver's gap-bridge solve tries round(f)±1), never as
        /// an integer to apply.
        double slip_wl_float{ std::numeric_limits<double>::quiet_NaN() };
    };

    /**
     * @brief Per-satellite MW arc quality summary for external consumers.
     *
     * Aggregates statistics across all MW pairs for one satellite into a
     * single quality snapshot suitable for pivot selection scoring.
     */
    struct SatelliteMwQuality {
        size_t min_arc_length{ 0 };   ///< Shortest arc (epoch count) across all pairs.
        double worst_sigma{ 1e9 };    ///< Largest MAD-sigma (meters) across all pairs (NaN if immature).
        bool   all_wl_locked{ false };///< True only if every BIA-corrected pair has a locked WL integer.
        size_t num_pairs{ 0 };        ///< Number of active MW pairs for this satellite.
    };

    /**
     * @class MwSlipDetector
     * @brief Detector for large MW (wide-lane / MW-like) discontinuities.
     *
     * The detector inspects per-satellite observations and computes an MW-like
     * quantity in meters. It keeps a short history (last value per satellite)
     * and flags ambiguities for reset when the MW change exceeds a configured
     * threshold (in wide-lane cycles).
     */
    class MwSlipDetector : public ISlipDetector {
    public:
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
         * @note @c CycleSlipManager does not call this — its specialized
         *       BSSD-aware pipeline consumes @c detectLargeMwResets directly
         *       for the full @c MwSlipResult WL-pair semantics the projection
         *       does not preserve.  This override exists for the polymorphic
         *       @c ISlipDetector contract.
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
         * @c detectLargeMwResets pass, it (a) is genuinely mature
         * (sample count ≥ 10 — a finite adaptive threshold is NOT used as
         * a maturity proxy, since the warm-up/cold-start branches return
         * finite thresholds for 2-sample arcs), (b) did not flag, (c) its
         * empirical MAD σ gives the test discriminating power
         * (3σ ≤ 0.7·λ_WL, so a real ±1-cycle wide-lane slip cannot be
         * pulled inside the clean bound by noise except as a ≥3σ event),
         * and (d) its sample deviation from the arc mean sat within
         * 0.3·@f$\lambda_{WL}@f$ — i.e. the wide-lane integer is
         * demonstrably unchanged at this epoch.  Key format matches the
         * arc stats key ("G01_L1C_L2W").
         *
         * Consumed by @c TurboEditResolver::resolve to gate its GF-only
         * equal-slip (@f$\Delta N_{WL}=0@f$) split: MW *silence* is not
         * sufficient evidence — a wl-locked arc deliberately absorbs a real
         * slip into its Welford stats without emitting a result, and an
         * immature arc could not have flagged at all.  Only affirmative
         * cleanliness justifies the equal-slip assumption.
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
         * Reports @c |mean/λ_WL − rounded_wl| + 3·σ_mean/λ_WL — the same
         * worst-case-frac expression the lock criterion uses, capturing
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
         * @brief Get all mature, non-slipped MW arc keys.
         *
         * Returns every stats key that has an active MW arc with enough
         * epoch history (diff_window >= kMinEpochsForAdaptive) to have
         * detected a slip if one occurred.  Immature arcs are excluded
         * because their slip-detection is deferred � treating them as
         * "clean" would create false constraints in the multi-frequency
         * solver.  Slipped arcs are excluded because they are erased
         * immediately upon detection.
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
         * @brief Settling across-gap wide-lane estimate for a demoted arc.
         *
         * Produced after a locked-arc TD slip confirmation demotes and
         * restarts an arc (the across-gap event): the demoted pre-jump
         * level is stashed, and as the post-restart arc's Hatch mean
         * converges, (new mean − pre-jump level) / λ_WL re-estimates the
         * across-gap WL step with √n-averaged code noise — the
         * instantaneous jump float published at confirm time is biased by
         * the re-acquisition NL-code transient (observed several cycles
         * off), which is exactly what poisons integer-candidate selection
         * in the TurboEdit gap-bridge solve.
         *
         * @c stable applies the same empirical criterion as the WL lock
         * (@c |frac| + 3·σ_mean < 0.5 cycles with n ≥ 10): the estimate is
         * confidently roundable.  Consumed by @c TurboEditResolver's
         * WL-settle deferral via @c getAcrossGapWlEvidence.
         */
        struct AcrossGapWlEvidence {
            double float_cycles{ 0.0 };       ///< (post mean − pre level) / λ_WL.
            std::size_t post_count{ 0 };      ///< Post-restart arc sample count.
            double sigma_mean_cycles{ 0.0 };  ///< Std error of the mean (cycles).
            bool stable{ false };             ///< Confidently roundable (see doc).
        };

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
         * @brief Clear MW arc history for a specific satellite.
         *
         * Call this when an ambiguity is reset by an external mechanism (e.g.,
         * KF outlier handler) so the MW detector starts a fresh arc on the
         * next epoch. Without this call, stale pre-reset MW values would
         * contaminate the running statistics and mask subsequent slips.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         */
        void resetSatellite(const std::string& satellite) override;

        /**
         * @brief Clear MW arc histories that involve a specific (satellite,
         *        signal) — leaving arcs on signal pairs that don't include
         *        this signal intact.
         *
         * Finer-grained alternative to @c resetSatellite for the common case
         * where only one signal slipped on a multi-frequency satellite (e.g.
         * L1C slipped while L2W and L5X are clean — the L2W/L5X arc should
         * survive while L1C/L2W and L1C/L5X arcs are cleared).
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
         * arcs with no slip flags fired against pre-gap history.
         */
        void resetAll() override;

        /**
         * @brief Enable CSV tracing for MWWL diagnostics.
         *
         * Opens or creates the file "mwwl_trace.csv" in the specified directory.
         * Subsequent calls to detectLargeMwResets() will write per-satellite MW
         * observations to this file for offline visualization.
         *
         * @param log_dir Directory where the trace file will be created.
         * @return true if the file was successfully opened, false otherwise.
         */
        bool enableTracing(const std::filesystem::path& log_dir) override;

        /**
         * @brief Check if tracing is currently enabled.
         * @return true if the trace file is open and writable.
         */
        bool isTracingEnabled() const override { return mwwl_trace_.is_open(); }

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
         * Application sets this to the RINEX sampling interval when the
         * data rate is coarser than 5 s and IMU is not available, so any
         * gap longer than the nominal cadence resets the arc rather than
         * silently absorbing a slip-during-gap into the running mean.
         * When IMU bridges gaps the default 120 s is preserved by not
         * calling this setter.
         *
         * @param seconds  New gap tolerance.  Values ≤ 0 are ignored
         *                 (default is preserved).
         */
        void setMaxGapSeconds(double seconds);

        /**
         * @brief Query aggregated MW arc quality for a satellite.
         *
         * Scans all MW arc stats keyed by the given satellite prefix and
         * returns the worst-case sigma, shortest arc length, and WL lock
         * status across all pairs.  Used by the BSSD pivot manager for
         * quality-aware pivot selection.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @return SatelliteMwQuality summary (num_pairs == 0 if no arcs exist).
         */
        SatelliteMwQuality getSatelliteMwQuality(const std::string& satellite) const;

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
        double max_gap_seconds_{ kDefaultMaxGapSeconds };

        /** Fixed capacity of the per-arc local-median ring buffer (raw MW
         *  values, meters).  The effective window length is the configured
         *  @c CycleSlipConfig::mw_local_window (≤ this cap; the config loader
         *  clamps).  A fixed @c std::array — rather than a deque — keeps the
         *  ring portion of @c MwArcStats trivially copyable so the per-epoch
         *  @c arc_snapshots_ copy and the rollback in @c resetSatellite /
         *  @c resetSatelliteSignal stay cheap. */
        static constexpr size_t kMwLocalWindowCap = 64;

        /**
         * @brief Per-satellite running statistics for the MW time series.
         *
         * Maintains a recursively updated mean and Welford variance estimate,
         * plus a rolling window of recent epoch-to-epoch MW differences used
         * to compute the Median Absolute Deviation (MAD) for robust outlier
         * detection. Using differences instead of raw values ensures the MAD
         * measures the actual noise level rather than spread around the
         * (arbitrary) ambiguity-dominated mean.
         *
         * Additionally holds a short ring buffer of recent accepted raw MW
         * values (@c mw_window) for the local-median detection reference
         * (@c MwDetectReference::LocalMedian).  Both windows admit only
         * non-slip epochs; the ring is maintained in BOTH reference modes
         * (cheap) so the trace can show the would-be local reference even
         * when the legacy Hatch reference governs detection.  Because the
         * ring lives inside this struct, the gap reset
         * (@c stats = MwArcStats{}), the @c arc_snapshots_ rollback, and
         * the erase-on-slip path all keep it consistent for free.  Code
         * paths that shift the arc's reference frame by a constant (the
         * BSSD pivot S-transform and the in-house bias commit re-anchor)
         * must shift the ring contents alongside @c mean / @c prev_mw —
         * see @c shiftLocalWindow.
         */
        struct MwArcStats {
            double mean{ 0.0 };            /**< Running (Hatch) mean of MW values (meters). */
            double M2{ 0.0 };              /**< Welford accumulator for variance (m^2). */
            size_t count{ 0 };             /**< Number of MW samples accumulated. */
            double prev_mw{ 0.0 };         /**< Previous epoch MW value for differencing (meters). */
            double last_epoch_s{ 0.0 };    /**< Continuous GPS seconds of the last update (for gap detection). */
            std::deque<double> diff_window; /**< Rolling window of epoch-to-epoch MW differences (meters). */
            bool   bia_corrected{ false };  /**< True if both satellite BIA bias and BSSD receiver bias are removed (integer-ready). */
            int    rounded_wl{ 0 };         /**< Rounded WL integer once locked (only valid when bia_corrected). */
            bool   wl_locked{ false };      /**< True once the WL integer has been locked by rounding. */
            double lambda_wl{ 0.0 };        /**< Wide-lane wavelength (meters) cached for slip repair. */
            double last_dev{ 0.0 };         /**< Active-reference deviation at the most recent epoch (meters). */
            double last_sigma{ std::numeric_limits<double>::quiet_NaN() }; /**< Active-reference sigma at the most recent epoch (meters). */
            std::array<double, kMwLocalWindowCap> mw_window{}; /**< Ring of recent accepted raw MW values (meters); valid entries are [0, mw_window_size). */
            size_t mw_window_head{ 0 };    /**< Next ring write slot (wraps at the configured window length). */
            size_t mw_window_size{ 0 };    /**< Number of valid ring entries (≤ configured window length). */
            /** Previous epoch's first time-difference TD1 = mw[k−1] − mw[k−2]
             *  (meters); NaN until two samples exist.  Combined with the
             *  current TD1 it yields the second time-difference
             *  TD2 = TD1[k] − TD1[k−1] for MwTdOrder::Second.  Updated every
             *  epoch alongside @c prev_mw (including slip epochs — see the
             *  echo note on @c td2_window).  Frame handling: the bias-commit
             *  re-anchor is a true constant shift within one sample series,
             *  to which TDs are invariant — no fix-up needed.  The BSSD
             *  pivot S-transform is NOT a constant shift of one series (it
             *  swaps which pivot's samples are differenced away), so the
             *  S-transform re-references this field by subtracting the new
             *  pivot's own old-frame prev_diff — exact through the shared
             *  old-pivot terms, and essential after a slip-FORCED handover,
             *  where the old pivot's dN would otherwise echo into every
             *  surviving arc's TD2 at the handover epoch. */
            double prev_diff{ std::numeric_limits<double>::quiet_NaN() };
            /** Rolling window of second time-differences (meters), the MAD
             *  source for the MwTdOrder::Second gate — mirrors
             *  @c diff_window (which holds TD1) in admission rule (non-slip
             *  epochs only) and cap (@c kMwWindowSize).  Echo note: a real
             *  slip on a LOCKED arc produces a +dN·λ TD2 at the slip epoch
             *  and a −dN·λ echo one epoch later (prev_diff carries the
             *  slip-sized TD1 forward); a locked-arc NOISE outlier o echoes
             *  twice (+o, −2o, +o over three epochs).  All such epochs flag,
             *  all are absorbed, and none enters this window.  Unlocked arcs
             *  erase at the slip epoch, so the echoes never form. */
            std::deque<double> td2_window;
            /** Locked-arc TD confirmation state (TimeDifference modes only).
             *  When the TD gate fires on a LOCKED arc, the event is not
             *  silently absorbed (the memoryless TD statistic flags a real
             *  step exactly once — absorption would discard the only
             *  detection it will ever produce and leave the stale locked
             *  integer exposed to AR for the ~dozen epochs until drift
             *  demotion): instead this flag is set and the decision is
             *  deferred ONE epoch.  The next processed sample resolves it
             *  by comparing two level hypotheses — after a one-epoch noise
             *  OUTLIER the level returns to the pre-jump track; after a
             *  real STEP it stays on the post-jump track.  Step → demote
             *  the lock, emit an UNKNOWN-INTEGER slip (downstream reset),
             *  erase the arc.  Outlier → keep the lock and excise the
             *  outlier epoch from the TD chain.
             *
             *  The confirmation NEVER publishes a known integer: a deferred
             *  result is one epoch late, so (a) the pass-2 SD→ZD
             *  re-referencing would pair a flag-epoch SD jump with the
             *  RESOLUTION epoch's pivot ZD slip (wrong frame — a pivot slip
             *  would mint confidently wrong integers on every clean locked
             *  peer), (b) TurboEdit would pair the integer with the
             *  resolution epoch's GF value (the sign-flipped echo of the
             *  real jump), and (c) the KF post-fit / TurboEdit may already
             *  have acted at the flag epoch, so an additive repair at k+1
             *  risks double mutation.  Reset is the only safe deferred
             *  action (reset-over-repair).
             *
             *  Frame guards: pending is neither entered nor resolved across
             *  a frame discontinuity — a bia_corrected flip of the incoming
             *  sample falls back to the legacy absorb (the deviation is a
             *  frame artifact, not an event), and a pivot ZD slip at the
             *  flag epoch takes an IMMEDIATE unknown-integer slip instead
             *  of deferring (synchronous with the pass-2 re-referencing and
             *  CycleSlipManager's pivot-slip machinery, which operate on
             *  that epoch's results); a pivot ZD slip at the resolution
             *  epoch makes the hypotheses unclassifiable and likewise
             *  resolves to the unknown-integer slip.  While pending, the
             *  locked integer is withheld from getResolvedWlAmbiguities /
             *  getResolvedWlFractions.  Cleared by the pivot S-transform
             *  (the stored quantities are frame-bound); reset/gap/erase
             *  handling is free via the struct's normal lifecycle. */
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
             *  resolution: recomputation does NOT reproduce the firing gate
             *  for warmup-branch locks (the cross-sat EWMA floor updates
             *  every epoch and re-expands at the new elevation) nor for
             *  TD1-fallback flags inside TD2 order. */
            double td_confirm_thresh_m{ 0.0 };
            /** Flag-epoch detect_mode code (2 = TD1, 3 = TD2; a TD1-fallback
             *  flag inside TD2 order correctly tags 2). */
            int    td_confirm_mode{ 0 };
        };

        /** Per-satellite MW arc statistics keyed by satellite identifier. */
        std::map<std::string, MwArcStats> mw_stats_;

        /** Pre-update snapshot of each arc taken at the start of every
         *  @c processMwSample call (after gap detection, before any
         *  state-modifying processing).  Keyed identically to
         *  @c mw_stats_.  Consumed by @c resetSatellite /
         *  @c resetSatelliteSignal to roll back a single epoch's
         *  contribution when the KF post-fit residual check identifies
         *  a slip MW's own detector missed — preserves the arc's pre-
         *  slip noise statistics (mean, M2, diff_window, count) rather
         *  than wiping the entire arc and forcing a re-warmup.  On
         *  rollback we additionally clear @c wl_locked / @c rounded_wl
         *  since a KF-detected slip means the underlying ambiguity
         *  changed and the previously-locked integer is no longer
         *  valid.
         *
         *  Cleared at the top of @c detectLargeMwResets each epoch so
         *  the map only ever holds current-epoch snapshots.  Consumed
         *  (erased per arc) by reset functions.  Also cleared in
         *  @c resetAll.  When no snapshot exists for an arc the reset
         *  falls back to the legacy full-erase behavior. */
        std::map<std::string, MwArcStats> arc_snapshots_;

        /** Current BSSD pivot satellite per (constellation char + signal-pair key).
         *  Example key: "G_L1C_L2W".  When the pivot changes all SD arcs
         *  for that group are reset because the SD ambiguity changes. */
        std::map<std::string, std::string> sd_pivot_sat_;

        /** One-shot deprioritization hints for the next MW pivot pick.
         *  Populated by @c notifyPivotForcedHandover and consumed/cleared
         *  by the next call to @c detectLargeMwResets. */
        std::set<std::string> deprioritized_pivot_sats_;

        /** Cross-satellite MAD σ per (constellation × signal-pair) group,
         *  keyed by "<system_char>_<sig1>_<sig2>" (e.g. "G_L1C_L2W").
         *  Updated at the end of each @c detectLargeMwResets call from
         *  the previous epoch's pool of per-sat MW deviations.  Consumed
         *  by @c computeAdaptiveThreshold as one of the σ_floor_static
         *  sources — provides statistical strength even when individual
         *  arcs are still warming up (cold start: 10 GPS sats at epoch 2
         *  give MAD with 9 degrees of freedom vs. zero per-arc).
         *  Persists across epochs as an EWMA (factor @c cross_sat_ewma_alpha_,
         *  bootstrapped by the first finite MAD); cleared by @c resetAll.
         *  Lags by one epoch — the current epoch's pool isn't known until all
         *  sats are processed, so the floor uses the previous epoch's σ.  Lag
         *  AND smoothing are acceptable because the noise environment doesn't
         *  change epoch-to-epoch; the EWMA is what makes the warmup floor stable
         *  instead of tracking the thin cross-section's per-epoch MAD jitter. */
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
        std::map<std::string, DemotedArcLevel> demoted_levels_;
        /** Age limit (s) for a demoted-level stash. */
        static constexpr double kDemotedLevelMaxAgeS = 120.0;
        /** Minimum post-restart samples before the settle estimate is trusted. */
        static constexpr std::size_t kWlSettleMinEpochs = 10;

        /** Coarser cross-sat σ tiers for the breadth cascade in
         *  @c computeAdaptiveThreshold, derived from the same per-epoch
         *  deviation pool as @c cross_sat_sigma_m_.  @c cross_sat_sigma_cons_
         *  is keyed by single-char constellation ("G") and pools all
         *  band-pairs of that constellation; @c cross_sat_sigma_global_ pools
         *  every constellation EXCEPT GLONASS (its FDMA inter-channel noise is
         *  the least comparable, so it must not contaminate the estimate the
         *  clean constellations fall back on).  A sparse band-pair group falls
         *  back to its constellation tier, then (non-GLONASS only) to the
         *  global tier, then to the gross-slip gate.  Both persist across
         *  epochs and are cleared by @c resetAll. */
        std::map<std::string, double> cross_sat_sigma_cons_;
        double cross_sat_sigma_global_ = std::numeric_limits<double>::quiet_NaN();

        /** Minimum cross-sat pool size before MAD is statistically
         *  meaningful.  Below this we leave the previous epoch's σ in
         *  place (or no entry, if cold start) rather than computing
         *  from a degenerately small pool. */
        static constexpr size_t kMinCrossSatSamples = 5;

        /**
         * @brief Persistent per-(sat, signal-pair) phase-bias estimate.
         *
         * Used when @c GnssConfig::getInhouseBiasEstimation() is enabled
         * for arcs lacking external BIA bias correction.  An arc that
         * matures with its running mean within @c kBiasCommitMaxFrac of
         * an integer cycle, AND with σ_mean below @c kBiasCommitMaxSigmaCyc,
         * commits its residual offset as a stored bias.  The stored bias
         * is subtracted from incoming MW samples on subsequent arcs of
         * the same (satellite, signal-pair) so the new arc accumulates
         * an integer-near mean from epoch one and is eligible for the
         * normal lock criterion.
         *
         * Lifetime is decoupled from @c MwArcStats — the bias survives
         * arc resets (new arc inherits parent's stored bias), survives
         * cycle slips (the integer changed but the bias didn't), and is
         * S-transformed alongside arc means when the MW pivot changes.
         */
        struct EstimatedBias {
            double bias_m{ 0.0 };           /**< Stored bias in meters; subtract from raw mw_m to get integer-near. */
            double bias_var_m2{ 0.0 };      /**< Uncertainty (variance) in m². */
            size_t commit_count{ 0 };       /**< Number of arcs that have contributed via EWMA. */
            double last_update_epoch_s{ 0.0 }; /**< Continuous GPS seconds of the most recent update. */
        };
        std::map<std::string, EstimatedBias> sat_pair_bias_;

        /** Minimum arc count before an unbiased arc is eligible to commit
         *  a bias estimate.  Higher than @c kWlLockMinN because the commit
         *  is a one-shot integer decision (whereas the lock criterion has
         *  the k_σ statistical safety net that scales with √n). */
        static constexpr size_t kBiasCommitMinCount = 20;

        /** Maximum |frac| (in cycles) for an unbiased arc to commit a
         *  bias.  Tighter than @c kWlMaxFrac because we are deciding
         *  which integer the arc converged to without external corroboration;
         *  a sat with frac in the 0.20–0.30 zone may legitimately belong
         *  to either side and we'd rather wait for more evidence (or skip
         *  the satellite) than gamble. */
        static constexpr double kBiasCommitMaxFrac = 0.20;

        /** Maximum σ_mean (cycles) for commit.  Ensures the running mean
         *  is statistically well-determined before its residual is
         *  declared a hardware bias. */
        static constexpr double kBiasCommitMaxSigmaCyc = 0.05;

        /** EWMA blending coefficient for cross-arc bias updates.  A small
         *  value (slow EWMA) reflects that hardware biases drift on
         *  hours-to-days timescales, so a single arc shouldn't move the
         *  estimate much; many arcs aggregating through the EWMA refine it. */
        static constexpr double kBiasEwmaAlpha = 0.20;

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
        bool tryCommitOrUpdateBias(MwArcStats& stats,
                                    const std::string& stats_key,
                                    double lambda_wl, double epoch_s);

        /** Maximum number of recent MW values kept per satellite for MAD computation. */
        static constexpr size_t kMwWindowSize = 30;

        /** Maximum effective count used in the Hatch (recursive) mean.
         *  Once the arc exceeds this count the weighting denominator is
         *  clamped, giving new samples a minimum weight of 1/kMaxHatchCount.
         *  Without this cap the mean becomes nearly frozen on long arcs and
         *  cannot track level shifts that fall just below the detection
         *  threshold, producing the characteristic exponential-decay
         *  residual pattern reported in MW time-difference diagnostics. */
        static constexpr size_t kMaxHatchCount = 100;

        /**
         * @brief Minimum number of diff_window entries before MAD-σ is
         *        trusted as the adaptive threshold input.  MAD computed
         *        from only 5 diffs is statistically marginal — when
         *        those few diffs happened to be tight by luck the MAD
         *        collapsed to near-zero, producing the 1-cycle → 0.008-
         *        cycle threshold transition observed in MWWL traces.
         *        15 diffs gives MAD enough degrees of freedom to be
         *        representative.  During the warmup phase
         *        @c computeAdaptiveThreshold uses a max of the
         *        cross-sat MAD, a mode-specific per-arc spread term
         *        (Welford σ_emp in the level modes; the young TD MAD
         *        over the partial diff_window / td2_window in the
         *        TimeDifference modes — see the @c mode_sigma_m
         *        paragraph of @c computeAdaptiveThreshold), and the
         *        absolute @c kMinSigmaMeters floor — none of which
         *        require external calibration. */
        static constexpr size_t kMinEpochsForAdaptive = 15;

        /** Lower bound on the MAD-derived sigma (meters) used in
         *  @c computeAdaptiveThreshold.  Prevents the threshold from
         *  collapsing to near-zero when the diff_window happens to be
         *  unnaturally tight (e.g., a brief stretch of nearly-identical
         *  samples on a quiet, short arc).  Set to 1 cm — well below
         *  typical L1/L2 WL MAD (~5–15 cm), so it doesn't change behavior
         *  for the standard pairs, while keeping the threshold
         *  wavelength-independent and useful for ultra-long EWL pairs
         *  (e.g., L5X/L8X at λ ≈ 19.5 m) whose natural MAD can be
         *  millimeters and whose meaningful anomalies live at the
         *  centimeter scale. */
        static constexpr double kMinSigmaMeters = 0.01;

        /** Default maximum inter-epoch gap (seconds) before the arc is
         *  reset.  Override at runtime via @c setMaxGapSeconds — typical
         *  use is to tighten this to the RINEX sampling interval when
         *  IMU is not available, so a brief outage cannot silently
         *  initialize a fresh arc with a slip-corrupted baseline.
         *  When IMU is available and bridging gaps via INS propagation,
         *  the default is preserved so legitimate continuous arcs
         *  survive 60–120 s outages. */
        static constexpr double kDefaultMaxGapSeconds = 120.0;

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
         *  epoch-to-epoch.  Because the MW noise ENVELOPE itself varies slowly
         *  (the one-epoch-lag rationale on @c cross_sat_sigma_m_), each tier is
         *  updated as an EWMA — σ_t = α·MAD_epoch + (1−α)·σ_{t−1}, first finite
         *  MAD as bootstrap — instead of overwritten with the raw MAD.  Smaller
         *  α = heavier smoothing; α = 1 disables smoothing.  Purely empirical,
         *  no stochastic-model input. */
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
         *  the per-arc @c mw_window ring instead of the laggy Hatch mean, and
         *  the spread is the same window's MAD (×√2 for TD1-equivalent
         *  multiplier scale) — deviation and spread are then the same local
         *  quantity, so a smooth MW trend lifts both and cancels instead of
         *  accumulating a lag offset that false-fires the gate.  With
         *  @c TimeDifference there is no level reference at all: the deviation
         *  IS the MW time-difference (order per @c td_order_), gated by the
         *  MAD of the same time-difference history — memoryless, hence immune
         *  to trends AND curvature (the soft-quadratic signals that defeat
         *  even the local median), at the cost of √2 (TD1) / √6 (TD2) noise
         *  amplification.  Only the detection statistic is affected: the
         *  Hatch mean, WL lock, drift demotion, and the bias-commit
         *  machinery are mode-independent. */
        MwDetectReference detect_reference_;

        /** Differencing order for @c MwDetectReference::TimeDifference, from
         *  @c CycleSlipConfig::mw_td_order.  First: dev = TD1 = mw[k]−mw[k−1],
         *  spread = MAD(@c diff_window) — the very statistic the legacy mature
         *  branch already maintains, now self-consistently gating its own
         *  quantity.  Second: dev = TD2 = TD1[k]−TD1[k−1], spread =
         *  MAD(@c td2_window).  Ignored in the level-reference modes. */
        MwTdOrder td_order_;

        /** Effective local-median window length (epochs), from
         *  @c CycleSlipConfig::mw_local_window clamped to
         *  [@c kMinEpochsForAdaptive, @c kMwLocalWindowCap]. */
        size_t local_window_len_;

        // Note: the WL lock criterion is no longer a fixed-N wait.  It's
        // an adaptive 3σ confidence test on the running mean,
        //
        //     |frac| + 3 · σ_mean / λ_WL  <  0.5    AND    |frac| < kWlMaxFrac
        //
        // implemented inline in @c processMwSample.  Clean arcs lock as
        // early as n=3; noisy arcs wait until √n has averaged the noise
        // down enough.  See the inline kWlLockKSigma / kWlLockMinN
        // constants there.

        /** Hard cap on raw fractional residual |mean/λ_WL − round(...)|
         *  for an MW arc to be exposed to AR (or to lock).  The
         *  statistical 3σ test alone can satisfy itself that a non-
         *  integer running mean (e.g. 3.33 with σ_mean = 0.05) is
         *  well-determined and round to a (potentially wrong) integer.
         *  This cap treats a large raw |frac| as a "real bias" signal
         *  that disqualifies the arc regardless of statistical
         *  confidence.  Used both at lock time and as a filter in
         *  @c getResolvedWlAmbiguities / @c getResolvedWlFractions. */
        static constexpr double kWlMaxFrac = 0.30;

        /** Live-mean drift threshold (WL cycles) that demotes a locked
         *  arc back to unlocked.  After lock the Welford / Hatch mean
         *  is no longer pinned to @c rounded_wl × λ_WL — it keeps
         *  tracking, so persistent disagreement between the running
         *  mean and the locked integer manifests as drift.  When
         *  |mean/λ − rounded_wl| exceeds this threshold the lock is
         *  no longer supported by accumulated evidence (either the
         *  initial lock was premature, or the arc has absorbed enough
         *  slipped samples to migrate); demote and let the arc re-
         *  resolve.  0.4 cycles is well inside the rounding basin
         *  (< 0.5) while leaving comfortable headroom over the lock
         *  criterion's kWlMaxFrac=0.30 ceiling, so a freshly locked
         *  arc with frac near the lock cap doesn't immediately
         *  demote itself on the next sample. */
        static constexpr double kWlLockDriftMaxCyc = 0.4;

        /**
         * @brief Compute the adaptive slip-detection threshold for an arc.
         *
         * Two regimes, with a clean warmup→per-arc handover:
         *
         *  - MATURE arc (diff_window ≥ @c kMinEpochsForAdaptive, so the per-arc
         *    MAD is trustworthy): threshold = @c sigma_multiplier_ ×
         *    max(σ_MAD, @c kMinSigmaMeters).  The per-arc per-signal MAD
         *    GOVERNS — the cross-sat σ is deliberately NOT applied as a floor
         *    here, because a single noisy cross-section should never override a
         *    converged per-arc estimate.  Only the absolute @c kMinSigmaMeters
         *    floor remains (anti-collapse).
         *  - WARMUP arc (no trustworthy per-arc MAD yet): threshold =
         *    @c sigma_multiplier_ × max(@c kMinSigmaMeters, cross-sat σ,
         *    mode-specific per-arc spread — Welford σ_emp in the level
         *    modes, the young TD MAD over the partial diff_window /
         *    td2_window in the TimeDifference modes; see the
         *    @c mode_sigma_m paragraph below).  The cross-sat σ (EWMA-smoothed, resolved
         *    by the breadth cascade band-pair → constellation →
         *    global(non-GLONASS), re-expanded to this arc's elevation) carries
         *    the cold-start epochs until the per-arc MAD takes over.  When NO
         *    cross-sat tier is populated (true cold start) the result is floored
         *    at a gross-slip GATE of min(λ_WL, 1 m) applied directly (not ×k).
         *
         * No stochastic-model-derived σ sources are consulted at any stage:
         * the gate is entirely data-driven, so a phone and a geodetic receiver
         * each converge to their own MW WL noise level automatically.
         *
         * The caller computes the mature spread of the ACTIVE detection
         * statistic (the statistic-selection site is @c processMwSample,
         * keeping this function statistic-agnostic) and passes it as
         * @c mode_sigma_m:
         *   - HatchMean: MAD(@c diff_window) — the legacy TD1 spread.
         *   - LocalMedian: the raw-MW ring's MAD ×√2 (TD1-equivalent scale).
         *   - TimeDifference/First: MAD(@c diff_window) — self-consistent,
         *     the deviation IS a TD1 sample.
         *   - TimeDifference/Second: MAD(@c td2_window).
         * When finite it GOVERNS the mature branch (×multiplier, floored at
         * @c kMinSigmaMeters).  When NaN (the statistic's window is still
         * immature) the WARMUP branch applies — cross-sat EWMA floor, a
         * per-arc spread term, @c kMinSigmaMeters, cold-start gross gate —
         * preserving the warmup-only cross-sat contract in every mode.  The
         * per-arc spread term is mode-specific: the level modes use the
         * Welford σ_emp (level-consistent; legacy bit-identical), while the
         * TimeDifference modes use the YOUNG TD MAD over the partial
         * diff_window / td2_window — σ_emp is a LEVEL variance that a trend
         * inflates by the ramp span (≈ S/√12), ballooning the warmup gate by
         * whole cycles against a noise-scale TD deviation.  The young MAD is
         * collapse-safe in floor position (it can lift the gate, never
         * undercut the other floors) and makes the warmup→mature handover
         * continuous in the statistic itself.  The cross-sat floor needs no
         * mode handling because its pool already holds active-statistic
         * deviations.
         *
         * @param stats     Current arc statistics for the satellite.
         * @param stats_key Per-arc key used to look up the cross-sat σ
         *                  floor for this (constellation × signal-pair)
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
        double computeAdaptiveThreshold(const MwArcStats& stats,
                                         const std::string& stats_key,
                                         double elev_deg,
                                         double mode_sigma_m =
                                             std::numeric_limits<double>::quiet_NaN()) const;

        /**
         * @brief Compute the MAD-derived Gaussian-equivalent sigma from an arc's diff window.
         * @param stats  Arc statistics whose diff_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeMadSigma(const MwArcStats& stats);

        /**
         * @brief MAD-derived Gaussian-equivalent σ of the arc's second
         *        time-difference window.
         *
         * Spread source for the @c MwTdOrder::Second gate — the same
         * @c kMinEpochsForAdaptive maturity floor as the TD1 MAD, so the
         * warmup→mature handover epoch is mode-independent.
         *
         * @param stats  Arc statistics whose td2_window is used.
         * @return Sigma in meters, or NaN if insufficient history.
         */
        static double computeTd2MadSigma(const MwArcStats& stats);

        /**
         * @brief White-noise scale of the active detection statistic
         *        relative to a raw LEVEL MW sample.
         *
         * 1 for the level references (HatchMean and LocalMedian — their
         * deviations, and hence the cross-sat pool they feed, are
         * level-domain), √2 for TimeDifference/First, √6 for
         * TimeDifference/Second.  Single source of truth for converting
         * between the level noise domain and the active-statistic domain.
         * Sole remaining consumer: the init-frac cross-sat lift divides by
         * it (the pool is active-domain, the gated init frac is
         * level-domain).  The warmup gate no longer scales σ_emp with it —
         * the TimeDifference modes replaced that floor term with the young
         * TD MAD, which lives natively in the active domain.
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
         * @c MwDetectReference::LocalMedian mode.  Causal by construction —
         * the ring holds samples through epoch k−1 only (the current sample
         * is pushed after the slip decision), so a slipped current epoch
         * cannot contaminate its own reference.
         *
         * The σ out-param is the same window's MAD × 1.4826 × √2.  The √2
         * keeps the configured @c mw_sigma_multiplier on its legacy scale:
         * the historical spread statistic is the MAD of first
         * time-differences, which for white noise estimates √2·σ_MW, while
         * the window MAD estimates σ_MW directly — without the rescale the
         * same multiplier would gate ~1.41× tighter in local mode and an
         * A/B against the legacy reference would conflate the reference
         * change with a threshold change.  σ is reported only once the ring
         * has ≥ @c kMinEpochsForAdaptive accepted samples (the same maturity
         * floor as the TD1 MAD); before that NaN is returned in the
         * out-param so the warmup floors keep governing the gate.
         *
         * @param stats             Arc whose ring buffer is summarized.
         * @param out_sigma_scaled  Optional; receives the √2-scaled MAD σ
         *                          (meters) or NaN while immature.
         * @return The window median (meters), or NaN if the ring is empty.
         */
        static double computeLocalReference(const MwArcStats& stats,
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
        void pushLocalWindow(MwArcStats& stats, double mw_m) const;

        /**
         * @brief Shift every valid sample in the arc's local ring by a
         *        constant (meters).
         *
         * Required wherever an arc survives a reference-frame jump and its
         * @c mean / @c prev_mw are shifted by a constant: the BSSD pivot
         * S-transform (shift by −old-pivot mean) and the in-house bias
         * commit re-anchor (shift by −committed bias).  Without the
         * matching ring shift, the epoch after the frame jump would show a
         * local deviation equal to the full frame offset — a guaranteed
         * false slip on every surviving arc.
         *
         * @param stats    Arc whose ring is shifted.
         * @param delta_m  Additive shift applied to each sample (meters).
         */
        static void shiftLocalWindow(MwArcStats& stats, double delta_m);

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
         * Reduces the satellite to its constellation prefix AND each signal to
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
         * diagnostic column), so the trace shows the floor even on mature arcs
         * where it no longer gates.
         *
         * @param stats_key Per-arc key (constellation × signal-pair) to pool by.
         * @param elev_deg  Arc elevation (deg) for the 1/sin re-expansion.
         * @return Cross-sat σ floor in meters, or NaN when no tier is populated
         *         (true cold start) or @p stats_key is empty.
         */
        double crossSatSigmaFloor(const std::string& stats_key,
                                  double elev_deg) const;

        /**
         * @brief Process a single MW sample: update statistics or flag a slip.
         *
         * Handles the first-epoch initialization, adaptive threshold evaluation,
         * trace writing, and Welford/Hatch statistics update for one MW value.
         *
         * For BIA+BSSD-corrected arcs the WL integer is locked on the very
         * first epoch (the MW is near-integer immediately).  When a slip is
         * detected on a locked arc and the new sample is also near-integer,
         * the integer slip magnitude ?N_WL is written to @p out_slip_cycles
         * so the caller can attempt joint MW+IF repair.
         *
         * @param obs            Observation (for tracing metadata).
         * @param stats_key      Key into mw_stats_ for this arc.
         * @param mw_m           Current MW value (meters), already bias-corrected by caller.
         * @param lambda_wl      Wide-lane wavelength (meters).
         * @param sig1           First signal identifier (for tracing).
         * @param sig2           Second signal identifier (for tracing).
         * @param bia_corrected  True if the MW value has had both satellite (BIA)
         *                       and receiver (BSSD) biases removed, making it
         *                       near-integer in WL cycles and eligible for rounding.
         * @param sat_mw_bias    Satellite MW bias that was applied (meters, for tracing).
         * @param[out] out_slip_cycles  If non-null, set to the integer WL slip magnitude.
         * @param[out] out_wl_known     If non-null, set to true when out_slip_cycles is reliable.
         * @param[out] out_slip_frac    If non-null, set to |x - round(x)| of the float value
         *                              before rounding (0 = exact, up to 0.5 = worst).
         * @param[out] out_slip_wl_float If non-null, set to the FLOAT WL-step
         *                              estimate in cycles where the emission path
         *                              has one (unlocked-arc jump deviation,
         *                              TD-confirmation level gap, drift-demotion
         *                              drift, level-disagreement gap), NaN
         *                              otherwise.  Same frame as the integer
         *                              outputs (SD for bias-corrected arcs —
         *                              the caller's SD→ZD re-referencing must be
         *                              applied identically); a candidate
         *                              SELECTOR for downstream solvers, never an
         *                              integer to apply.
         * @param pivot_sat      SD pivot satellite for this arc's (constellation,
         *                       signal-pair) group, recorded in the trace's
         *                       @c mw_pivot column.  Empty for undifferenced
         *                       (no-BIA) arcs that are not single-differenced
         *                       against any pivot.
         * @param pivot_zd_slip_in  The pivot's OWN ZD slip detected THIS pass
         *                       (cycles; 0 when the pivot is clean or the arc
         *                       is not single-differenced).  Consumed only by
         *                       the locked-arc TD confirmation: a non-zero
         *                       value marks the SD frame as moved this epoch,
         *                       which forbids both deferring a new pending
         *                       (the epoch's machinery must see the slip NOW)
         *                       and classifying an existing one (hypotheses
         *                       are old-frame).
         * @param pivot_zd_known_in Whether the pivot's ZD slip integer is
         *                       known; false likewise marks the frame moved.
         * @return True if a slip was detected (caller should generate reset keys).
         */
        bool processMwSample(const PppObservation& obs,
            const std::string& stats_key,
            double mw_m, double lambda_wl,
            const std::string& sig1, const std::string& sig2,
            bool bia_corrected, double sat_mw_bias,
            int* out_slip_cycles = nullptr,
            bool* out_wl_known = nullptr,
            double* out_slip_frac = nullptr,
            double* out_slip_wl_float = nullptr,
            const std::string& pivot_sat = {},
            int pivot_zd_slip_in = 0,
            bool pivot_zd_known_in = true);

        /** Output stream for MWWL trace CSV file (mutable for const access). */
        mutable std::ofstream mwwl_trace_;

        /** Current epoch time for tracing (set at start of detectLargeMwResets). */
        mutable GnssTime current_epoch_;

        /**
         * @brief Find ionosphere-free phase/code label pair for an observation.
         *
         * Scans the observation's observable labels and sets @p phase_if_key and
         * @p code_if_key when an ionosphere-free pair is found (for example
         * phase: "L1C_L2W", code: "C1C_C2W"). Returns true on success.
         *
         * @param obs Observation to inspect.
         * @param[out] phase_if_key Output ionosphere-free phase label (if found).
         * @param[out] code_if_key Output ionosphere-free code label (if found).
         * @return true when a matching ionosphere-free pair was found.
         * @return false otherwise.
         */
        bool findIonoFreeLabels(const PppObservation& obs, std::string& phase_if_key, std::string& code_if_key) const;

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
         * @brief Compute the Melbourne-W�bbena Wide-Lane (MWWL) observable in meters.
         *
         * Computes the full MW combination:
         *   MW = (f1�L1 - f2�L2)/(f1-f2) - (f1�P1 + f2�P2)/(f1+f2)
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
         *   b_MW = (f1�b_L1 - f2�b_L2)/(f1-f2) - (f1�b_C1 + f2�b_C2)/(f1+f2)
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
         *                 the ring is empty).  Emitted in both reference
         *                 modes so a legacy-mode run still records what the
         *                 local reference would have been (offline A/B).
         * @param local_sigma_m √2-scaled local-window MAD σ (meters; NaN
         *                 while the ring is immature).
         * @param detect_mode Statistic that governed this row's slip
         *                 decision: 0 = Hatch-mean level deviation,
         *                 1 = local-median level deviation (also used for
         *                 the level-disagreement backstop rows, whose
         *                 quantities are synthesized), 2 = first
         *                 time-difference, 3 = second time-difference.
         *                 Rows that bypass the dev test (arc-init, LLI)
         *                 carry 0.
         * @param active_dev_m The deviation the detector actually gated
         *                 this row (meters): dev_from_mean in mode 0, the
         *                 local/TD deviation in modes 1–3, the synthesized
         *                 level gap on backstop rows.  NaN on rows that
         *                 bypass the dev test.  Makes every row
         *                 self-describing — |active_dev|/threshold > 1 iff
         *                 is_slip, in every mode.
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
        };

} // namespace GnssProcessor