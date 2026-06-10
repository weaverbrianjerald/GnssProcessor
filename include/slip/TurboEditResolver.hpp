#pragma once

/**
 * @file TurboEditResolver.hpp
 * @brief Melbourne-Wübbena + geometry-free joint cycle-slip *repair* resolver
 *        (Blewitt TurboEdit) — shared by the post-processing and real-time paths.
 *
 * @par Motivation
 * The Melbourne-Wübbena (MW) wide-lane combination measures only the wide-lane
 * difference @f$\Delta N_{WL}=\Delta N_1-\Delta N_2@f$: one equation in two
 * unknowns.  For a satellite tracked on 3+ frequencies @c CycleSlipManager can
 * already cross-check overlapping MW pairs to isolate the culprit signal, but
 * for the common **dual-frequency** case MW alone cannot attribute a slip to a
 * single signal, so the only safe response is a full ambiguity reset (which
 * discards the converged ambiguity precision).  MW is also blind to the
 * @f$\Delta N_1=\Delta N_2@f$ case, where the wide-lane is unchanged.
 *
 * TurboEdit closes both gaps by adding the geometry-free (GF) phase combination
 * as the missing second equation:
 * @f[
 *    \Delta N_{WL}  = \Delta N_1 - \Delta N_2              \quad\text{(MW, integer)}
 * @f]
 * @f[
 *    \Delta\Phi_{GF} = \lambda_1\,\Delta N_1 - \lambda_2\,\Delta N_2
 *                                                          \quad\text{(GF jump, metres)}
 * @f]
 * solved for the per-signal integers
 * @f[
 *    \Delta N_2 = \frac{\Delta\Phi_{GF} - \lambda_1\,\Delta N_{WL}}{\lambda_1-\lambda_2},
 *    \qquad \Delta N_1 = \Delta N_2 + \Delta N_{WL}.
 * @f]
 * @f$\Delta\Phi_{GF}@f$ is estimated by the second-order time difference of the
 * geometry-free phase, @f$GF(t)-2\,GF(t\!-\!1)+GF(t\!-\!2)@f$, which cancels the
 * ionospheric delay up to quadratic order so that, at the slip epoch, it equals
 * the slip jump plus only phase noise.
 *
 * @par High-rate requirement (intrinsic)
 * The cancellation only holds while the ionospheric delay is slowly varying
 * relative to the sampling interval — i.e. at high rate (≤1 s).  At coarser
 * rates the ionospheric residual contaminates the GF jump and the per-signal
 * split becomes unreliable.  The resolver therefore (a) logs a one-time warning
 * when handed an interval > @c kHighRateMaxIntervalS, and (b) gates every epoch
 * on a cross-satellite ionospheric-variability index (IVI): when the ionosphere
 * is disturbed the GF jump is not trusted and flagged satellites fall back to a
 * reset.  See @c project_vertical_instability_solidtide / the @c GfSlipDetector
 * banner for the iono-contamination rationale.
 *
 * @par Separation of concerns
 * This class is a *pure resolver*: it owns the per-arc GF jump history and the
 * MW+GF arithmetic, and returns integer per-signal repairs plus a reset-fallback
 * set.  It does **not** mutate filter state and is unaware of BSSD framing — the
 * caller applies the integers using its own state convention (the post-
 * processing @c CycleSlipManager feeds them through the BSSD-aware repair /
 * pivot-handover machinery; @c RealTimeEngine applies a covariance-preserving
 * additive shift for non-pivot satellites and resets otherwise).  The MW
 * wide-lane integers are taken as input (from @c MwSlipDetector) rather than
 * recomputed, so the empirically-tuned MW detection path remains the single
 * source of @f$\Delta N_{WL}@f$.
 *
 * @note The GF combination arithmetic here is intentionally self-contained.  It
 *       mirrors the (currently staged) @c GfSlipDetector and the active
 *       @c IonoVariabilityMonitor; consolidating the three onto one shared
 *       GF-combination helper is a worthwhile future cleanup but is out of scope
 *       for this component.
 */

#include "common/PppTypes.hpp"
#include "config/GnssConfig.hpp"
#include "corrections/GnssCorrections.hpp"
#include "logging/LogManager.hpp"
#include "slip/MwSlipDetector.hpp"   // MwSlipResult (input)
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @class TurboEditResolver
 * @brief Resolves dual-frequency cycle slips into per-signal integer repairs by
 *        pairing the MW wide-lane integer with the geometry-free phase jump.
 */
class TurboEditResolver {
public:
    /**
     * @brief A confident per-(satellite, signal) integer slip repair.
     *
     * @c dN is the zero-differenced integer cycle slip in @em this signal's
     * cycles; the caller applies @c dN·λ as an additive, covariance-preserving
     * shift to the corresponding carrier-phase ambiguity (BSSD pivots excepted).
     */
    struct SignalRepair {
        std::string satellite; ///< Satellite identifier (e.g. "G01").
        std::string signal;    ///< Signal identifier (e.g. "L1C").
        int    dN{ 0 };        ///< Integer slip in this signal's cycles (never 0 in emitted repairs).
        double lambda{ 0.0 };  ///< Signal wavelength (m).
        double frac{ 0.0 };    ///< Rounding residual of the float estimate (0 = exact, ≤0.5 worst).
    };

    /**
     * @brief Per-epoch resolution result.
     */
    struct EpochResolution {
        std::vector<SignalRepair> repairs;   ///< Confident per-signal integer repairs to apply.
        std::set<std::string>     reset_sats; ///< Satellites flagged but unresolved — caller should reset.
        bool   iono_stable{ true };          ///< False ⇒ GF jumps untrusted this epoch (IVI gate tripped).
        double max_ivi{ 0.0 };               ///< Worst-case cross-satellite IVI this epoch (m).
        bool   rate_ok{ true };              ///< False ⇒ sampling interval exceeded the high-rate limit.
    };

    /**
     * @brief Construct the resolver.
     * @param config      Processing configuration (elevation mask, etc.).
     * @param log_manager Logging facility.
     */
    TurboEditResolver(const GnssConfig& config, LogManager& log_manager);

    /**
     * @brief Provide satellite BIA corrections.
     *
     * The GF combination is bias-free in its time-difference, so this is a
     * no-op today; retained for interface parity with the detectors and for
     * forward compatibility.
     *
     * @param corrections Bias source (may be nullptr); owned externally.
     */
    void setCorrections(const GnssCorrections* corrections) { corrections_ = corrections; }

    /**
     * @brief Resolve cycle slips for one epoch.
     *
     * Must be called every epoch (it rolls the GF jump arcs forward), even when
     * @p mw_results is empty — otherwise the GF history needed for the next
     * jump estimate is lost.
     *
     * @param mw_results   MW slip results from @c MwSlipDetector::detectLargeMwResets
     *                     for this epoch (the source of @f$\Delta N_{WL}@f$).
     * @param observations Modeled observations for this epoch (raw carrier phase, cycles).
     * @param interval_s   Effective sampling interval (s); ≤0 means unknown (the
     *                     rate warning is then suppressed).
     * @return Per-signal integer repairs and the reset-fallback set.
     */
    EpochResolution resolve(const std::vector<MwSlipResult>& mw_results,
                            const std::vector<PppObservation>& observations,
                            double interval_s);

    /**
     * @brief Clear all GF arc state for a satellite (every signal pair).
     * @param satellite Satellite identifier (e.g. "G01").
     */
    void resetSatellite(const std::string& satellite);

    /**
     * @brief Clear GF arc state for pairs involving a specific (satellite, signal).
     * @param satellite Satellite identifier (e.g. "G01").
     * @param signal    Signal identifier (e.g. "L1C").
     */
    void resetSatelliteSignal(const std::string& satellite, const std::string& signal);

    /**
     * @brief Drop all GF arc state (hard-gap restart).
     */
    void resetAll();

    /**
     * @brief Override the inter-epoch gap tolerance before a GF arc is reset.
     * @param seconds New gap tolerance; values ≤ 0 are ignored.
     */
    void setMaxGapSeconds(double seconds);

    /**
     * @brief Enable CSV tracing of the per-pair joint solve.
     * @param log_dir Directory in which to create @c turbo_edit_trace.csv.
     * @return true if the trace file was opened.
     */
    bool enableTracing(const std::filesystem::path& log_dir);

    /// @brief Whether tracing is currently active.
    bool isTracingEnabled() const { return trace_file_.is_open(); }

private:
    const GnssConfig& config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;
    const GnssCorrections* corrections_{ nullptr };

    /// Effective inter-epoch gap tolerance (s); overridable via @c setMaxGapSeconds.
    double max_gap_seconds_{ kDefaultMaxGapSeconds };

    /// One-shot guard so the coarse-rate warning is logged only once per run.
    bool rate_warned_{ false };

    /// Continuous GPS seconds of the previous epoch that carried observations.
    /// The 2nd-order time difference requires three *consecutive* epochs, so an
    /// arc whose last update predates this (it sat out the previous epoch — a
    /// per-signal dropout) is non-consecutive and must be restarted, regardless
    /// of the absolute @c max_gap_seconds_ tolerance.  0 until the first epoch.
    double prev_epoch_s_{ 0.0 };

    /**
     * @brief A dual-frequency carrier-phase pair, ordered higher-frequency first.
     */
    struct PhasePair {
        std::string phase1;  ///< Higher-frequency phase signal (e.g. "L1C").
        std::string phase2;  ///< Lower-frequency phase signal (e.g. "L2W").
    };

    /**
     * @brief Per-(satellite, phase-pair) geometry-free jump arc.
     */
    struct GfArc {
        double prev_gf{ 0.0 };           ///< GF at epoch t-1 (m).
        double prev_prev_gf{ 0.0 };      ///< GF at epoch t-2 (m).
        double last_epoch_s{ 0.0 };      ///< Continuous GPS seconds of last update.
        std::size_t count{ 0 };          ///< Epochs accumulated in this arc.
        std::deque<double> diff_window;  ///< Recent TD2 values (m) for the MAD threshold.
        std::size_t cooldown_remaining{ 0 }; ///< Anti-echo cooldown after a slip (epochs).
    };

    /// Per-(satellite + phase-pair) GF jump arcs.  Key: "G01_L1C_L2W".
    std::map<std::string, GfArc> gf_arcs_;

    // --- Tuning constants ------------------------------------------------

    /// Rolling window length for the MAD-based GF-only detection threshold.
    static constexpr std::size_t kGfWindowSize = 30;
    /// Minimum TD2 samples before the adaptive MAD threshold is trusted.
    static constexpr std::size_t kMinEpochsForAdaptive = 5;
    /// MAD multiplier for the GF-only (blind-spot) detection threshold.
    static constexpr double kMadSigmaMultiplier = 6.0;
    /// Floor on the GF-only detection threshold (m).  Below |λ1−λ2| (~0.054 m
    /// for GPS L1/L2) so a ±1-cycle equal slip (MW's blind spot) is detectable.
    static constexpr double kMinThresholdM = 0.04;
    /// Maximum rounding residual of the float ΔN2 to accept the integer split.
    static constexpr double kMaxFractional = 0.30;
    /// Default inter-epoch gap (s) before a GF arc is reset.
    static constexpr double kDefaultMaxGapSeconds = 120.0;
    /// Anti-echo cooldown after a slip (epochs): the TD2 echo of a prior event
    /// must not retrigger a GF-only detection.
    static constexpr std::size_t kCooldownEpochs = 3;
    /// Cross-satellite IVI threshold (m): above this the ionosphere is deemed
    /// active and GF jumps are not trusted.  Matches @c GfSlipDetector.
    static constexpr double kIviThreshold = 0.06;
    /// Sanity bound on a resolved per-signal integer slip.
    static constexpr int kMaxSlipCycles = 1000;
    /// Sampling interval (s) above which the GF jump is iono-contaminated and a
    /// one-time warning is emitted.
    static constexpr double kHighRateMaxIntervalS = 1.0;

    // --- GF combination helpers (self-contained; see file note) ----------

    /**
     * @brief Collect every dual-frequency carrier-phase pair from an observation,
     *        ordered higher-frequency first.
     *
     * All tracked phase attributes are paired (not one canonical per band) so
     * that whatever (sig1, sig2) the MW detector reported has a matching GF arc.
     */
    std::vector<PhasePair> collectPhasePairs(const PppObservation& obs) const;

    /**
     * @brief Geometry-free phase observable @f$L_1-L_2@f$ in metres.
     * @return GF value (m), or NaN on missing/invalid input.
     */
    double computeGfMeters(const PppObservation& obs,
                           const std::string& sig1,
                           const std::string& sig2) const;

    /// @brief MAD-derived Gaussian-equivalent sigma of a TD2 window (NaN if immature).
    static double computeMadSigma(const std::deque<double>& window);

    /// @brief Adaptive GF-only detection threshold (m) from an arc's TD2 window.
    double computeAdaptiveThreshold(const GfArc& arc) const;

    /**
     * @brief Cross-satellite ionospheric-variability index for a constellation.
     * @param constellation_td2 Per-arc |TD2| values for one constellation this epoch.
     * @return IVI (m): median(|TD2|) + MAD-sigma(|TD2|); 0 if insufficient data.
     */
    static double computeIvi(const std::vector<double>& constellation_td2);

    /// @brief Build the GF arc key "SAT_SIG1_SIG2".
    static std::string arcKey(const std::string& sat,
                              const std::string& sig1,
                              const std::string& sig2);

    /// Output stream for the per-pair joint-solve CSV trace.
    mutable std::ofstream trace_file_;
    /// Epoch time captured for tracing.
    GnssTime current_epoch_;
};

} // namespace GnssProcessor
