/**
 * @file AmbLevelCheck.hpp
 * @brief Consumer-side re-validation of float ambiguities against their own
 *        code level at the re-coupling edge of invariant #7: windowed levels,
 *        the two tests, the trip decision and its counters.
 *
 * Cards CC and PBT establish that a preserved ambiguity is continuous; neither
 * establishes that it is correct.  On the RF-0hv field bed, mass resets
 * re-levelled accumulated ambiguity error from code as a side effect and left
 * the ambiguities nearer the truth than the arm that preserved them (PBT §7).
 * This check performs that re-levelling deliberately, with both edges of
 * invariant #7 instrumented.
 *
 * ## The statistic
 *
 * Schematically, the consumer's rows are (MeasurementModel::buildJacobianRow)
 *
 *     predicted_phase = modeled + geometry + clock + tropo - iono
 *                       + ssr_bias_term + amb          (amb in metres)
 *     predicted_code  = modeled + geometry + clock + tropo + iono
 *                       + ssr_bias_term
 *
 * so the pre-fit innovation of each row is the observation minus that: the
 * corrected phase or code the model formed, with the satellite clock, the
 * satellite code bias and the satellite phase bias already applied, referenced to
 * the current state.  Two classical combinations of the four innovations
 * (L1 phase, L2 phase, L1 code, L2 code) measure ambiguity-level inconsistency
 * together with any remaining bias, multipath and model errors:
 *
 *   WL (Melbourne-Wubbena), cycles; geometry-free and ionosphere-free:
 *       mw = [ (f1*ip1 - f2*ip2)/(f1-f2) - (f1*ic1 + f2*ic2)/(f1+f2) ] / lambda_WL
 *          = (N_WL from code) - (N_WL from the float states)  + noise
 *
 *   IF (ionosphere-free), metres; geometry-free and ionosphere-free:
 *       ifd = (f1^2*(ip1-ic1) - f2^2*(ip2-ic2)) / (f1^2 - f2^2)
 *           = (IF level from code) - (IF combination of the float ambiguities)
 *             + noise
 *
 * Both are formed from the consumer's own corrected observations (the products
 * are not re-read) as paired per-epoch level-minus-state differences.
 * Common geometry, receiver clock and troposphere terms cancel, as does
 * first-order slant ionosphere with the modeled frequency dependence.  A
 * phase-only receiver clock does not cancel, and neither do arbitrary code or
 * phase correction errors, so a trip alone does not identify ambiguity error as
 * the cause.
 *
 * ## The test
 *
 * Once a satellite's window holds @c window_ep samples:
 *
 *     |mean(mw)|  > k * sigma_WL     with sigma_WL = max(s_mw/sqrt(n), 0.02 cyc)
 *     |mean(ifd)| > k * sigma_IF     with sigma_IF = max(s_ifd/sqrt(n), 0.05 m)
 *
 * Here sigma is a floored estimated standard error, not a calibrated ambiguity
 * uncertainty.  With independent Gaussian zero-mean samples and inactive floors,
 * mean/(s/sqrt(n)) follows Student t(n-1), not a standard normal distribution.
 * Temporal correlation, changing state estimates, shared WL/IF measurements,
 * overlapping checks and data-dependent restarts need separate calibration.
 * The floors bound the rejection bar; they do not model these uncertainties.
 * See docs/refound/ALC-STAT_statistics_audit_2026-09-17.md for the audit.
 *
 * ## The window restart (invariant #7, and why a translated epoch cannot trip)
 *
 * This class restarts a window only when told to (@ref restart).  The engine
 * owns the restart rule, and the guarantee lives there: see
 * @c RealTimeEngine::checkAmbiguityLevel_ (RealTimeEngineEstimate.cpp).
 *
 * The engine compares persistent StateManager generations for both selected
 * ambiguities with the last sampled pair, so a reset stays visible even after
 * reseeding and after a KF update has shrunk its variance.  It also restarts the
 * window when either ambiguity state is missing or its variance is at or above
 * the reset variance.  Explicit satellite notifications clear windows
 * immediately.  A trip also clears the window, in both modes: in @c act the
 * refloat advances the ambiguity generations; in @c observe the clear makes the
 * counters record distinct trip events instead of one trip per epoch until the
 * drift ages out of the window.
 *
 * Counter closure (standing invariant #4): every full-window evaluation is
 * exactly one of {no trip, WL trip, IF trip}, so
 * @c trips_wl + trips_if <= checks, and in @c act mode @c refloats equals the
 * number of trips (one refloat per tripped satellite, both bands).
 */
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace GnssProcessor {

/// One AMB_LEVEL row for `network_events.csv`, at either edge of invariant #7.
struct AmbLevelEvent {
    std::string sat;                  ///< Satellite the event is about.
    const char* kind{ "trip" };       ///< "trip" (the decoupling test fired) or "refloat" (the response).
    const char* test{ "" };           ///< "wl" | "if" for a trip; the tripping test for the refloat it caused.
    double      diff{ 0.0 };          ///< |state - level| (WL cycles, or IF metres).
    double      sigma{ 0.0 };         ///< sigma_level actually used (same unit).
    int         n{ 0 };               ///< Window sample count behind the level.
    std::string detail;               ///< Free-form identities (signals, k, the other test's value).
};

/// Windowed code levels, the two tests and the counters; pure arithmetic.
class AmbLevelCheck {
public:
    /// Action taken on a trip.
    enum class Mode {
        Act,      ///< Refloat both of the satellite's ambiguities (the default).
        Observe   ///< Count only; no state is written anywhere.
    };

    /// Which test fired.
    enum class Test { None, Wl, If };

    /// The three knobs, resolved.
    struct Config {
        int    window_ep{ 20 };        ///< Samples per satellite before the test runs (20 = 10 min at 30 s).
        double k{ 4.0 };               ///< Trip at |state - level| > k * sigma_level.
        Mode   mode{ Mode::Act };      ///< Act (refloat) or Observe (count).
    };

    /// Result of one @ref push.
    struct Verdict {
        bool   tested{ false };        ///< The window was full, so a test ran (a "check").
        Test   test{ Test::None };     ///< Which test tripped, if any.
        double diff{ 0.0 };            ///< |mean| of the tripping statistic.
        double sigma{ 0.0 };           ///< sigma_level of the tripping statistic.
        int    n{ 0 };                 ///< Samples behind it.
        double wl_mean{ 0.0 };         ///< Both statistics, always reported (the trace carries the non-tripping one too).
        double wl_sigma{ 0.0 };
        double if_mean{ 0.0 };
        double if_sigma{ 0.0 };
    };

    /// Run-end counters (the ON-path telemetry of invariant #4).
    struct Counters {
        uint64_t samples{ 0 };    ///< (satellite, epoch) samples pushed into a window.
        uint64_t checks{ 0 };     ///< n_amb_level_checks: full-window evaluations.
        uint64_t trips_wl{ 0 };   ///< n_amb_level_trips_wl.
        uint64_t trips_if{ 0 };   ///< n_amb_level_trips_if.
        uint64_t refloats{ 0 };   ///< n_amb_level_refloats (satellites; two ambiguities each).
        uint64_t restarts{ 0 };   ///< Non-empty windows cleared by the engine's restart rule.
        uint64_t sats{ 0 };       ///< Distinct satellites that ever held a window.
    };

    /// sigma_level floor, WL test (cycles); see the class banner.
    static constexpr double kSigmaFloorWlCyc = 0.02;
    /// sigma_level floor, IF test (metres); see the class banner.
    static constexpr double kSigmaFloorIfM = 0.05;

    /// Resolve the knobs (called once, from the engine's first armed epoch).
    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    /**
     * @brief Clear @p sat's window at a translation, carry, reset or birth epoch.
     * @return true iff a non-empty window existed and was cleared (counted in
     *         @c restarts).
     */
    bool restart(const std::string& sat);

    /**
     * @brief Append this epoch's paired (level - state) sample and test.
     *
     * @param sat    Satellite id.
     * @param wl_cyc The WL statistic for this epoch (cycles); see @ref wlLevelMinusState.
     * @param if_m   The IF statistic for this epoch (metres); see @ref ifLevelMinusState.
     * @return The verdict; @c tested is false while the window is still filling.
     *
     * WL is tested first: a wide-lane error is the coarser, integer-bearing one,
     * and an IF trip on the same satellite in the same epoch would report the
     * same defect twice.  A trip clears the window (see the class banner).
     */
    Verdict push(const std::string& sat, double wl_cyc, double if_m);

    /// Record that a trip was acted on (the engine refloated both bands).
    void noteRefloat() { ++ctr_.refloats; }

    const Counters& counters() const { return ctr_; }

    /// Live window depth for @p sat (0 when it holds none), for tests and traces.
    std::size_t depth(const std::string& sat) const;

    /**
     * @brief WL (Melbourne-Wubbena) level minus state, in cycles, from the four
     *        pre-fit innovations of one satellite.
     *
     * Geometry-, clock- and ionosphere-free: the WL phase coefficients
     * (f1, -f2)/(f1-f2) and the NL code coefficients (f1, f2)/(f1+f2) each sum to
     * 1 on the geometry and cancel the first-order ionosphere against each other.
     *
     * @param f1,f2      Carrier frequencies (Hz), f1 != f2.
     * @param lambda_wl  c / (f1 - f2) (m).
     * @param ip1,ip2    Pre-fit phase innovations, band 1 / band 2 (m).
     * @param ic1,ic2    Pre-fit code innovations, band 1 / band 2 (m).
     */
    static double wlLevelMinusState(double f1, double f2, double lambda_wl,
                                    double ip1, double ip2, double ic1, double ic2) {
        const double wl_phase = (f1 * ip1 - f2 * ip2) / (f1 - f2);
        const double nl_code  = (f1 * ic1 + f2 * ic2) / (f1 + f2);
        return (wl_phase - nl_code) / lambda_wl;
    }

    /**
     * @brief Ionosphere-free level minus state, in metres, from the same four
     *        pre-fit innovations: the IF combination of the ambiguity errors.
     */
    static double ifLevelMinusState(double f1, double f2,
                                    double ip1, double ip2, double ic1, double ic2) {
        const double g1 = f1 * f1, g2 = f2 * f2;
        return (g1 * (ip1 - ic1) - g2 * (ip2 - ic2)) / (g1 - g2);
    }

    /// Stable name of a test for logs, events and tests.
    static const char* testName(Test t);

private:
    struct Window {
        std::deque<double> wl;   ///< WL statistic samples (cycles).
        std::deque<double> ifd;  ///< IF statistic samples (metres).
    };
    /// Mean and sample standard error of the mean of @p v, floored at @p floor.
    static void moments(const std::deque<double>& v, double floor,
                        double& mean, double& sigma);

    Config   cfg_{};
    std::map<std::string, Window> win_;
    Counters ctr_{};
};

} // namespace GnssProcessor
