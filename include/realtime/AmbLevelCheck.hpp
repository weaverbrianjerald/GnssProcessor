/**
 * @file AmbLevelCheck.hpp
 * @brief Card ALC: the consumer RE-VALIDATES its float ambiguities against their
 *        own code level at invariant #7's RE-COUPLING edge.  Windowed levels,
 *        the two tests, the trip decision and its counters.
 *
 * Cards CC and PBT both established that a preserved ambiguity is *continuous*.
 * Neither established that it is *correct*.  Measured on the RF-0hv field bed
 * (PBT §7): over TOW 270900-273000 the reset-only baseline moved its ambiguities
 * by a cumulative 3.2416 m through two mass resets and came out NEARER the truth,
 * while the translated arm moved 0.4489 m and came out 0.53 / 1.45 m worse at
 * hours 4 / 5.  The resets were re-levelling accumulated ambiguity error FROM
 * CODE, by accident.  This card does the same thing deliberately, with both of
 * invariant #7's edges instrumented.
 *
 * ## The statistic
 *
 * The consumer's rows are (MeasurementModel.cpp:1640-1643)
 *
 *     predicted_phase = modeled + geometry + clock + tropo - iono
 *                       + ssr_bias_term + amb          (amb in METRES)
 *     predicted_code  = modeled + geometry + clock + tropo + iono
 *                       + ssr_bias_term
 *
 * so the PRE-fit innovation of each row is the observation minus that — i.e. the
 * corrected phase / code the model actually formed, with the satellite clock, the
 * satellite code bias and the satellite phase bias already applied, referenced to
 * the current state.  Two classical combinations of those four innovations
 * (L1 phase, L2 phase, L1 code, L2 code) measure ambiguity-level inconsistency
 * together with any surviving bias, multipath and model errors:
 *
 *   WL (Melbourne-Wubbena), cycles — geometry-free AND ionosphere-free:
 *       mw = [ (f1*ip1 - f2*ip2)/(f1-f2) - (f1*ic1 + f2*ic2)/(f1+f2) ] / lambda_WL
 *          = (N_WL from code) - (N_WL from the float states)  + noise
 *
 *   IF (ionosphere-free), metres — geometry-free AND ionosphere-free:
 *       ifd = (f1^2*(ip1-ic1) - f2^2*(ip2-ic2)) / (f1^2 - f2^2)
 *           = (IF level from code) - (IF combination of the float ambiguities)
 *             + noise
 *
 * Both are formed from the CONSUMER'S OWN corrected observations — the products
 * are never re-read — as PAIRED per-epoch level-minus-state differences.
 * Common geometry, receiver clock and troposphere terms cancel, as does
 * first-order slant ionosphere with the modeled frequency dependence. A
 * PHASE-ONLY receiver clock does not cancel. Neither do arbitrary code/phase
 * correction errors. A trip alone cannot identify ambiguity error as the cause.
 *
 * ## The test
 *
 * Once a satellite's window holds @c window_ep samples:
 *
 *     |mean(mw)|  > k * sigma_WL     with sigma_WL = max(s_mw/sqrt(n), 0.02 cyc)
 *     |mean(ifd)| > k * sigma_IF     with sigma_IF = max(s_ifd/sqrt(n), 0.05 m)
 *
 * Here sigma is a FLOORED ESTIMATED standard error, not a calibrated ambiguity
 * uncertainty. With independent Gaussian zero-mean samples and inactive floors,
 * mean/(s/sqrt(n)) follows Student t(n-1), not a standard normal distribution.
 * Temporal correlation, changing state estimates, shared WL/IF measurements,
 * overlapping checks and data-dependent restarts need separate calibration.
 * The floors bound the rejection bar; they do not model these uncertainties.
 * See docs/refound/ALC-STAT_statistics_audit_2026-09-17.md for the audit.
 *
 * ## The window restart (invariant #7, and why a translated epoch cannot trip)
 *
 * This class only restarts when told to (@ref restart); the ENGINE owns the rule
 * and is where the guarantee lives — see
 * @c RealTimeEngine::checkAmbiguityLevel_ (RealTimeEngineEstimate.cpp).
 *
 * The engine checks persistent StateManager generations for both selected
 * ambiguities: a reset remains visible even after reseeding and a KF update
 * shrink its variance. Explicit satellite notifications clear windows immediately.
 * A trip also clears the window, in BOTH modes. In @c act the refloat advances
 * the ambiguity generations; in @c observe clearing counts distinct trip
 * EVENTS instead of one trip per epoch until the drift ages out of the window.
 *
 * Counter closure (standing invariant #4): every full-window evaluation is
 * exactly one of {no trip, WL trip, IF trip}, so
 * @c trips_wl + trips_if <= checks, and in @c act mode @c refloats == the number
 * of trips (one refloat per tripped satellite, both bands).
 */
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace GnssProcessor {

/// Card ALC: one AMB_LEVEL row for `network_events.csv` (invariant #7's edges).
struct AmbLevelEvent {
    std::string sat;                  ///< Satellite the event is about.
    const char* kind{ "trip" };       ///< "trip" (the decoupling test fired) or "refloat" (the response).
    const char* test{ "" };           ///< "wl" | "if" for a trip; the tripping test for the refloat it caused.
    double      diff{ 0.0 };          ///< |state - level| (WL cycles, or IF metres).
    double      sigma{ 0.0 };         ///< sigma_level actually used (same unit).
    int         n{ 0 };               ///< Window sample count behind the level.
    std::string detail;               ///< Free-form identities (signals, k, the other test's value).
};

/// Card ALC: windowed code levels + the two tests + counters (pure arithmetic).
class AmbLevelCheck {
public:
    /// What a trip DOES.
    enum class Mode {
        Act,      ///< Refloat both of the satellite's ambiguities (the default).
        Observe   ///< Count only — no state is written anywhere.
    };

    /// Which test fired.
    enum class Test { None, Wl, If };

    /// The three knobs, resolved.
    struct Config {
        int    window_ep{ 20 };        ///< Samples per satellite before the test is legal (20 = 10 min at 30 s).
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
        double wl_mean{ 0.0 };         ///< Both statistics, always reported (the trace carries the loser too).
        double wl_sigma{ 0.0 };
        double if_mean{ 0.0 };
        double if_sigma{ 0.0 };
    };

    /// Run-end counters (invariant #4's ON-path telemetry).
    struct Counters {
        uint64_t samples{ 0 };    ///< (satellite, epoch) samples pushed into a window.
        uint64_t checks{ 0 };     ///< n_amb_level_checks — full-window evaluations.
        uint64_t trips_wl{ 0 };   ///< n_amb_level_trips_wl.
        uint64_t trips_if{ 0 };   ///< n_amb_level_trips_if.
        uint64_t refloats{ 0 };   ///< n_amb_level_refloats (satellites; two ambiguities each).
        uint64_t restarts{ 0 };   ///< Windows dropped by the engine's restart rule.
        uint64_t sats{ 0 };       ///< Distinct satellites that ever held a window.
    };

    /// sigma_level floor, WL test (cycles) — see the class banner.
    static constexpr double kSigmaFloorWlCyc = 0.02;
    /// sigma_level floor, IF test (metres) — see the class banner.
    static constexpr double kSigmaFloorIfM = 0.05;

    /// Resolve the knobs (called once, from the engine's first armed epoch).
    void configure(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    /**
     * @brief Drop @p sat's window — a translation / carry / reset / birth epoch.
     * @return true iff a window actually existed and was dropped (counted).
     */
    bool restart(const std::string& sat);

    /**
     * @brief Append this epoch's paired (level - state) sample and test.
     *
     * @param sat    Satellite id.
     * @param wl_cyc The WL statistic for this epoch (cycles) — see @ref wlLevelMinusState.
     * @param if_m   The IF statistic for this epoch (metres) — see @ref ifLevelMinusState.
     * @return The verdict; @c tested is false while the window is still filling.
     *
     * WL is tested first: a wide-lane error is the coarser, integer-bearing one
     * and an IF trip on the same satellite in the same epoch would name the same
     * defect twice.  A trip clears the window (see the class banner).
     */
    Verdict push(const std::string& sat, double wl_cyc, double if_m);

    /// Record that a trip was acted on (the engine refloated both bands).
    void noteRefloat() { ++ctr_.refloats; }

    const Counters& counters() const { return ctr_; }

    /// Live window depth for @p sat (0 when it holds none) — tests and traces.
    std::size_t depth(const std::string& sat) const;

    /**
     * @brief WL (Melbourne-Wubbena) level minus state, in CYCLES, from the four
     *        pre-fit innovations of one satellite.
     *
     * Geometry-, clock- and ionosphere-free: the WL phase coefficients
     * (f1, -f2)/(f1-f2) and the NL code coefficients (f1, f2)/(f1+f2) each sum to
     * 1 on the geometry and cancel the first-order ionosphere against each other.
     *
     * @param f1,f2      Carrier frequencies (Hz), f1 != f2.
     * @param lambda_wl  c / (f1 - f2) (m).
     * @param ip1,ip2    Pre-fit PHASE innovations, band 1 / band 2 (m).
     * @param ic1,ic2    Pre-fit CODE innovations, band 1 / band 2 (m).
     */
    static double wlLevelMinusState(double f1, double f2, double lambda_wl,
                                    double ip1, double ip2, double ic1, double ic2) {
        const double wl_phase = (f1 * ip1 - f2 * ip2) / (f1 - f2);
        const double nl_code  = (f1 * ic1 + f2 * ic2) / (f1 + f2);
        return (wl_phase - nl_code) / lambda_wl;
    }

    /**
     * @brief Ionosphere-free level minus state, in METRES, from the same four
     *        pre-fit innovations — the IF combination of the ambiguity errors.
     */
    static double ifLevelMinusState(double f1, double f2,
                                    double ip1, double ip2, double ic1, double ic2) {
        const double g1 = f1 * f1, g2 = f2 * f2;
        return (g1 * (ip1 - ic1) - g2 * (ip2 - ic2)) / (g1 - g2);
    }

    /// Stable name of a test for logs / events / tests.
    static const char* testName(Test t);

private:
    struct Window {
        std::deque<double> wl;   ///< WL statistic samples (cycles).
        std::deque<double> ifd;  ///< IF statistic samples (metres).
    };
    /// mean and sample-sigma-of-the-mean of @p v, floored at @p floor.
    static void moments(const std::deque<double>& v, double floor,
                        double& mean, double& sigma);

    Config   cfg_{};
    std::map<std::string, Window> win_;
    Counters ctr_{};
};

} // namespace GnssProcessor
