#pragma once

/**
 * @file MwSlipTypes.hpp
 * @brief The value types the MW slip detector hands to its callers: the per-ambiguity
 *        result and the values of its query methods.
 *
 * Owned by @c MwSlipDetector (slip/MwSlipDetector.hpp, which includes this header, so
 * no caller includes it directly).  The class publishes the nested types under the
 * names its callers use (@c MwSlipDetector::WlLockQuality and the others); the
 * definitions live here once.  Plain value types: no behaviour, no virtual dispatch.
 *
 * Two groups: the types every driver reads (the result and the WL-lock and across-gap
 * queries), then the network-only types (the per-epoch pair status with its LLI shadow,
 * the zero-differenced MW arc reads and the phase-bridge reads), which only the network
 * producer consumes and which card NOM owns with it.
 */

#include <cstddef>
#include <limits>
#include <string>

namespace GnssProcessor {

    /**
     * @brief Result of MW slip detection for a single ambiguity.
     *
     * The MW observable measures N_WL = N_1 - N_2.  A WL slip shows that this
     * difference changed; it cannot show which signal slipped.
     * When slip_wl_known is true, slip_wl_cycles contains an integer
     * estimate of the WL jump, and slip_frac records how far from integer
     * the raw value was before rounding (0 = exact, up to 0.5 = worst).
     *
     * @note slip_wl_cycles is always the satellite's own undifferenced
     *       (zero-differenced) WL slip.  Bias-corrected arcs are detected in a
     *       single-differenced frame against the detector's internal MW pivot,
     *       but that pivot's own slip is added back before reporting, so
     *       consumers may difference two satellites' slips directly without
     *       knowing (or sharing) the MW pivot.  When the conversion cannot be
     *       made (MW pivot slipped with an unresolved integer) slip_wl_known is
     *       set false so the consumer falls back to a reset.
     */
    struct MwSlipResult {
        std::string amb_key;           ///< Ambiguity key (e.g. "G01_L1C").
        int    slip_wl_cycles{ 0 };    ///< Satellite's own (zero-differenced) WL slip dN_WL, cycles (0 if unknown).
        bool   slip_wl_known{ false }; ///< True if slip_wl_cycles is a usable integer.
        double lambda_wl{ 0.0 };       ///< Wide-lane wavelength (meters) for this signal pair.
        std::string sig1;              ///< First signal (higher freq, e.g. "L1C").
        std::string sig2;              ///< Second signal (lower freq, e.g. "L2W").
        std::string satellite;         ///< Satellite identifier (e.g. "G01").
        double slip_frac{ 0.0 };       ///< Fractional residual |x - round(x)| before rounding
                                       ///< (0 = exact integer, 0.5 = worst).  Clean pairs and
                                       ///< BIA-locked pairs have ~0; best-effort pairs may be large.
        /// Float estimate of the WL slip (cycles, same zero-differenced frame
        /// as @c slip_wl_cycles).  Populated wherever the emission path has a
        /// meaningful step estimate, including when @c slip_wl_known is false:
        /// for example the locked-arc TD-confirmation level gap, whose integer
        /// is withheld by contract (the deferred path never publishes one) but
        /// whose magnitude is sound.  NaN when no estimate exists or when the
        /// SD→ZD re-referencing could not be applied (pivot slip unresolved).
        /// Consumers use it only to select integer candidates (for example,
        /// TurboEditResolver's gap-bridge solve tries round(f)±1), never as an
        /// integer to apply.
        double slip_wl_float{ std::numeric_limits<double>::quiet_NaN() };

        // --- Detection strength (mw_evidence_refloat) -----------------------
        // Three quantities a detect-only consumer uses to judge a flag that it
        // cannot judge by integer cleanliness.  slip_frac grades how cleanly the
        // step rounds, not how far above the noise it stood, so a large slip
        // flagged far above its threshold can still fail a frac gate.  Every
        // slip-detection emission path reports them (LLI resets keep the
        // defaults), and no detector decision depends on them.
        // The network producer's MW refloat admission reads them and admits on
        // them only with network.cycle_slip.mw_evidence_refloat on.

        /// |deviation| / threshold at the flag: the firing statistic as a
        /// multiple of its own adaptive threshold.  NaN where the emitting path
        /// has no meaningful ratio (none is fabricated).
        double evidence_ratio{ std::numeric_limits<double>::quiet_NaN() };
        /// Arc maturity: the arc's accumulated sample count at the flag, before
        /// the erase.  0 on the init-suspicious path (the arc is one sample old
        /// by construction) and wherever the emitting path has no arc.
        int    arc_epochs{ 0 };
        /// Lane class: true when the MW value the flag was formed on had both the
        /// satellite (BIA) and the receiver (single-difference) bias removed.
        /// Such a lane is single-differenced against the detector's internal MW
        /// pivot, so a pivot S-transform re-reference appears as a synchronous
        /// multi-satellite "slip" at one station; that is the measured
        /// false-alarm population.  The group's MW pivot also reports true: its
        /// value is BIA-corrected and it is the reference of that S-transform.
        /// Evidence-based admission is therefore restricted to lanes with
        /// bia_corrected false.
        bool   bia_corrected{ false };
    };

    /**
     * @brief Standard error of one locked wide-lane arc mean, the value
     *        type of @ref MwSlipDetector::getResolvedWlQuality (named
     *        @c MwSlipDetector::WlLockQuality there).
     */
    struct MwWlLockQuality {
        double      sigma_mean_cycles{ 0.0 };  ///< σ of the arc mean (cycles).
        std::size_t count{ 0 };                ///< Welford sample count backing that σ.
        bool        exact{ false };            ///< SD-vs-self pivot: 0 cycles by construction, no uncertainty.
    };

    /**
     * @brief Settling across-gap wide-lane estimate for a demoted arc.
     *
     * Produced after a locked-arc TD slip confirmation demotes and
     * restarts an arc (the across-gap event): the demoted pre-jump
     * level is stashed, and as the post-restart arc's Hatch mean
     * converges, (new mean − pre-jump level) / λ_WL re-estimates the
     * across-gap WL step with √n-averaged code noise.  The instantaneous
     * jump float published at confirm time is biased by the
     * re-acquisition NL-code transient (observed several cycles off),
     * which misleads integer-candidate selection in the TurboEdit
     * gap-bridge solve.
     *
     * @c stable applies the same empirical criterion as the WL lock
     * (@c |frac| + 3·σ_mean < 0.5 cycles with n ≥ 10): the estimate is
     * confidently roundable.  Consumed by @c TurboEditResolver's
     * WL-settle deferral via @c getAcrossGapWlEvidence.
     */
    struct MwAcrossGapWlEvidence {
        double float_cycles{ 0.0 };       ///< (post mean − pre level) / λ_WL.
        std::size_t post_count{ 0 };      ///< Post-restart arc sample count.
        double sigma_mean_cycles{ 0.0 };  ///< Std error of the mean (cycles).
        bool stable{ false };             ///< Confidently roundable (see doc).
    };

    // ==========================================================================
    // Network-only value types, owned with card NOM
    //
    // Read only by the network producer (the common-band scope, the MW wide-lane
    // layer, the lane carry); the user-side drivers never call the accessors that
    // return them.
    // ==========================================================================

    /**
     * @brief What the detector did with one arc this epoch: the
     *        detection-grade record, as opposed to the cleanliness-grade
     *        record of @c getEpochCleanPairs.
     */
    struct MwEpochPairStatus {
        /// Arc sample count as the flag decision saw it (samples before this
        /// one; the Welford increment happens later in the same call).
        int    arc_n_at_sample{ 0 };
        /// The detector's own flag verdict for this sample.  Advisory: a
        /// consumer deciding "did this pair flag" should use the emitted
        /// @c MwSlipResult set, which is what every downstream drain acts on.
        bool   flagged{ false };
        /// Adaptive detection threshold at this sample (metres).  NaN when the
        /// sample left before a threshold existed (an arc's very first epoch).
        double thresh_m{ std::numeric_limits<double>::quiet_NaN() };
        /// Wide-lane wavelength of the pair (m).  Dividing @ref thresh_m by it
        /// gives the threshold in cycles, comparable with a slip magnitude.
        double lambda_wl{ 0.0 };
        /// LLI shadow (SPEC_event_harness.md §14.11): one of this pair's bands
        /// carried a receiver loss-of-lock bit at the previous epoch, so the
        /// pair's previous sample was withheld.  The uncombined LLI branch of
        /// @ref detectLargeMwResets schedules the ambiguity resets and skips
        /// the satellite (`continue`) while preserving the MW arc on purpose:
        /// the MW combination depends on raw observations alone, so a short
        /// LLI gap does not invalidate the arc statistics.  The arc is not
        /// reset at the flagged epoch, and this sample, the first after it,
        /// is compared against a mean that never saw the epoch the receiver
        /// declared broken.  A band-common slip leaves the wide lane unmoved,
        /// so the lane reads as silent and mature while carrying no
        /// information about that epoch.  On the storm bed, every spurious
        /// `keep` verdict came one epoch after an LLI, on a lane that
        /// contained the flagged band.
        bool   lli_shadow{ false };
    };

    /**
     * @brief A per-arc Melbourne-Wubbena Welford summary in a ZD-consistent frame.
     *
     * The frame reconstruction (see @ref arcZdMean) makes @c mean_m a
     * zero-differenced per-(station,satellite) MW value regardless of whether the
     * arc is stored single-differenced (a non-pivot BIA arc) or undifferenced (a
     * pivot arc / no-BIA fallback), so a sign-weighted sum around a closed
     * station-satellite cycle cancels the receiver and satellite hardware biases
     * exactly (M5 card WL-1; see @c NetworkMwWlLayer).
     */
    struct MwArcZdMean {
        double      mean_m{ std::numeric_limits<double>::quiet_NaN() };        ///< ZD-consistent MW mean (m), lagged one epoch (SPEC_mw_wl_card §3.3 double-count guard).
        double      var_of_mean_m2{ std::numeric_limits<double>::quiet_NaN() };///< Welford variance of the lagged mean = M2_lag/(n_lag(n_lag−1)) (m²); for a reconstructed SD arc this is a conservative (over-counted) upper bound, because the common pivot variance cancels in a closed cycle.
        std::size_t count{ 0 };                                                ///< Lagged Welford sample count (min across the arc and, if reconstructed, its pivot).
        double      lambda_wl{ 0.0 };                                          ///< cached wide-lane wavelength (m).
        bool        bia_corrected{ false };                                    ///< satellite-bias frame of the underlying arc (true = BIA-subtracted).  Cycle-level satellite-bias cancellation requires a uniform frame, so the WL layer skips mixed-frame cycles.
        bool        valid{ false };                                            ///< the arc exists, has ≥2 lagged samples, and a finite ZD mean/variance was recovered.
        /// The (satellite, signal-pair) arc key is present in the accumulator,
        /// i.e. the detector currently holds an arc (of any age) on this exact
        /// pair.  Strictly weaker than @c valid, and the only way for a caller
        /// to tell "this pair was never built" (a band the detector does not
        /// pair, or a tracking channel it did not choose) from "built but not
        /// yet usable" (young, or an SD arc whose group pivot is missing).  The
        /// two cases have different causes and fixes; a single invalid flag
        /// would let an EWL consumer report "immature arc" for a whole run on a
        /// pair that does not exist.
        bool        exists{ false };
        /// The RINEX code observables the MW combination consumed on
        /// (@c sig1, @c sig2): @c matchChannel's same-tracking-attribute pick, with
        /// its in-band fallback (see @ref collectUncombinedPairs).  Served because a
        /// consumer that models the arc's satellite code bias must name the same
        /// per-signal OSB column the observable carries.  Deriving "C"+band+attribute
        /// from the phase key is right only when the fallback did not fire, and a
        /// mis-attributed OSB column is a modelling error nothing downstream detects.
        /// Empty on an arc fed before this was recorded (never within one run).
        std::string code1, code2;
    };

    /**
     * @brief A per-arc lagged per-epoch MW sample in the ZD-consistent frame: the
     *        per-epoch increment source of the lane-coordinate carry (WL-3).
     *
     * This is not the Welford mean (@ref arcZdMean): under the lane-coordinate
     * carry the estimator owns the accumulation, so each epoch's raw sample must
     * enter exactly once.  The served value is the raw MW of the arc's previous
     * processed epoch, with the same one-epoch lag as @c mean_lag (F1), so a
     * same-epoch consumer never re-ingests the observations the estimator is
     * folding this epoch.  @c var_m2 is the per-sample Welford variance
     * M2_lag/(n_lag−1) (m²): the per-epoch noise, not the variance of the mean.
     * For a reconstructed SD arc the value adds the group pivot's own lagged raw
     * sample, and the variance adds both arcs' per-sample variances (conservative,
     * because the common pivot term cancels in a closed DD cycle).  The
     * reconstruction requires the arc and its pivot to have sampled the same
     * epoch (checked via @c epoch_s, else @c valid=false).
     */
    struct MwArcZdLast {
        double      value_m{ std::numeric_limits<double>::quiet_NaN() };  ///< ZD-consistent lagged raw MW sample (m).
        double      var_m2{ std::numeric_limits<double>::quiet_NaN() };   ///< Per-sample Welford variance (m²); conservative for SD-reconstructed arcs.
        double      epoch_s{ 0.0 };                                       ///< Continuous GPS seconds at which the sample was taken; the caller's cycle-consistency guard (all arcs of a DD cycle must share it).
        std::size_t count{ 0 };                                           ///< Lagged Welford sample count backing @c var_m2.
        double      lambda_wl{ 0.0 };                                     ///< Cached wide-lane wavelength (m).
        bool        bia_corrected{ false };                               ///< Frame flag; mixed-frame cycles must be skipped (as in @ref MwArcZdMean).
        bool        valid{ false };                                       ///< A finite lagged sample + variance was recovered (needs ≥2 lagged samples).
    };

    /**
     * @brief A per-arc geometry-free WL↔EWL phase bridge Welford summary.
     *
     * The bridge observable @f$GF_b=\phi_{WL}-\phi_{EWL}@f$ (meters, phase only, no
     * code) is geometry-free: the geometry, clock and tropo terms cancel between
     * the two lane phases, leaving @f$\lambda_{WL}N_{WL}-\lambda_{EWL}N_{EWL}@f$
     * (constant along the arc) plus a slow differential-iono residue and phase
     * noise.  The raw phases carry the receiver and satellite hardware biases, so
     * @f$GF_b@f$ is stored undifferenced (ZD) per (station, sat); those biases
     * cancel in the closed DD cycle the layer forms, so no MW-style SD pivot is
     * needed (unlike @ref MwArcZdMean).  The layer double-differences
     * @f$\sum c\,GF_b@f$ over the Card-4 chord basis and adds
     * @f$\lambda_{EWL}\,\nabla\Delta N_{EWL}^{\text{fixed}}@f$ to obtain a fast,
     * iono-limited WL integer determination: @f$z_{WL}^{\text{bridge}}=[\sum
     * c\,GF_b+\lambda_{EWL}N_{EWL}]/\lambda_{WL}@f$.
     */
    struct MwBridgeMean {
        double      gf_mean_m{ std::numeric_limits<double>::quiet_NaN() };        ///< ZD @f$GF_b@f$ mean (m), lagged one epoch (same double-count guard as @ref MwArcZdMean).
        double      var_of_mean_m2{ std::numeric_limits<double>::quiet_NaN() };   ///< Welford variance of the lagged mean = M2_lag/(n_lag(n_lag−1)) (m²).  Reflects the differential-iono drift and phase noise (read by the §3.4 v1 iono σ-guard).
        std::size_t count{ 0 };                                                   ///< Lagged Welford sample count.
        double      lambda_wl{ 0.0 };                                             ///< cached WL wavelength c/(f1−f2) (m).
        double      lambda_ewl{ 0.0 };                                            ///< cached EWL wavelength c/(f2−f5) (m).
        double      iono_coeff{ 0.0 };                                            ///< @f$GF_b@f$ sensitivity to the L1-referenced slant iono @f$I_1@f$ (m/m): @f$C_{iono}=f_{ref}^2(\tfrac1{f_1 f_2}-\tfrac1{f_2 f_5})@f$, @f$f_{ref}=@f$ GPS L1 (the estimator's iono-state frame, MeasurementModel §Jacobian).  Without the per-sample de-bias (@ref setBridgePerSampleDebias) the consumer subtracts @f$C_{iono}\sum c\,\hat I_1@f$ to de-bias the bridge (SPEC §3.4 v2; the v1 Welford-only form fails under a stable differential iono, whose near-zero variance passes the σ-guard yet biases the rounded integer).
        bool        valid{ false };                                              ///< the arc exists, has ≥2 lagged samples, and a finite mean/variance/λ was recovered.
    };

    /**
     * @brief A bridge arc's lagged per-epoch raw @f$GF_b@f$ sample: the
     *        phase-precision increment source of the lane-coordinate carry
     *        (WL-3, SPEC_lane_carry_card.md §3.2).  Mirrors @ref MwArcZdLast:
     *        raw sample and per-sample variance, not the Welford mean, with the
     *        same one-epoch lag (F1) and once-only use.
     */
    struct MwBridgeLast {
        double      gf_m{ std::numeric_limits<double>::quiet_NaN() };   ///< Lagged raw ZD @f$GF_b@f$ sample (m).
        double      var_m2{ std::numeric_limits<double>::quiet_NaN() }; ///< Per-sample Welford variance M2_lag/(n_lag−1) (m²).
        double      epoch_s{ 0.0 };                                     ///< Continuous GPS seconds the sample was taken (cycle-consistency guard).
        std::size_t count{ 0 };                                         ///< Lagged Welford sample count.
        double      lambda_wl{ 0.0 };                                   ///< WL wavelength (m).
        double      lambda_ewl{ 0.0 };                                  ///< EWL wavelength (m).
        double      iono_coeff{ 0.0 };                                  ///< @f$GF_b@f$ iono sensitivity (see @ref MwBridgeMean::iono_coeff).
        bool        valid{ false };                                     ///< A finite lagged sample + variance was recovered.
    };

} // namespace GnssProcessor
