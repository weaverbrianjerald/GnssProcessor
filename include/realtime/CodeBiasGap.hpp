#pragma once
/**
 * @file CodeBiasGap.hpp
 * @brief The consumer's response to a code signal whose satellite code bias is missing from the
 *        stream: the gap's two edges, the code rows left out, the satellites barred from a new
 *        fix, and the test of a held wide lane against the code once the bias returns.
 *
 * ## The defect it answers
 *
 * The stream carries a satellite's code biases as one record that replaces the previous one, so a
 * signal the network leaves out of an epoch has no bias at the consumer from that epoch on, and the
 * consumer applies the code with a bias of zero.  The network leaves a signal out whenever fewer
 * than two of its stations carry that signal (a code-bias column needs two), which happens to the
 * second Galileo signal of the eccentric-orbit satellites and of newly launched ones.  A missing
 * bias of 2.13 m on E5a moves the code's wide lane by 1.2 cycles.  While a satellite's ambiguities
 * are tight the code moves them little; when they are loose (a new arc, or a reset by a network
 * phase-bias reset) the code is the only information on the wide lane, and a fix made then takes
 * the neighbouring wide lane with the matching narrow lane.  For Galileo E1/E5a that step, (3, 4)
 * cycles, changes the range by 5.6 mm and the slant ionosphere by 0.56 m, so neither the position
 * nor the residuals object to it.  Observed at ARHT00ATA on 26 and 27 May 2026 (E18, E32).
 *
 * ## The response (key processing.ssr_code_bias_gap_response; network.closed_loop.code_bias_gap_response)
 *
 * A (satellite, code signal) is IN A GAP at an epoch when the stream carries the satellite (it has
 * a fresh code-bias record) but that record holds no bias for the signal.  Both edges are events
 * with counters and identities (standing invariant 7):
 *
 *  - Open edge (the bias goes missing).  Mode @c full: the signal's code row is left out of the
 *    update while the gap is open, and the satellite's ambiguities that are not held are barred
 *    from the integer search (no new fix of that satellite).  A hold taken before the gap on
 *    clean code is kept.  Mode @c revalidate writes nothing at this edge.
 *  - Close edge (the bias returns).  Both modes open the satellite's RE-VALIDATION window of
 *    @c window_ep epochs.  Every hold of the satellite present in the window is tested over
 *    @c revalidate_ep consecutive epochs of clean code: the held wide lane against the receiver's
 *    own Melbourne-Wubbena corrected with the stream's biases, less the per-epoch median of the
 *    same quantity over the other held satellites of the system (the receiver's wide-lane bias and
 *    the frame's pivot cancel).  A mean beyond half a cycle releases the satellite (its
 *    ambiguities are re-floated); otherwise the hold is validated.  Mode @c full keeps the bar on
 *    a new fix for the first @c revalidate_ep epochs of the window, so a float carried through the
 *    gap first sees the returned code.
 *
 * Half a cycle is the rounding boundary of the tested quantity, not a tuned threshold: the hold is
 * released exactly when the code rounds to a different wide lane.
 *
 * This header holds the decisions, the arithmetic and the counters only; it is pure and unit-tested
 * on hand-built epochs (tests/test_code_bias_gap.cpp).  The engine side is
 * RealTimeEngineCodeBiasGap.cpp.
 */

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Gap edges, the re-validation windows and their counters, per (satellite, code signal).
 */
class CodeBiasGap {
public:
    /// @brief What the response does.
    enum class Mode {
        Off,         ///< Nothing is measured or written (the default; byte-identical).
        Revalidate,  ///< Close edge only: test the satellite's holds against the returned code, release on failure.
        Full         ///< Both edges: leave the bias-less code out, bar new fixes, then re-validate.
    };

    /// @brief The knobs, resolved.
    struct Config {
        Mode   mode{ Mode::Off };     ///< The response.
        int    revalidate_ep{ 20 };   ///< Clean held epochs behind a verdict (20 = 10 min at 30 s); also the bar's length after the return in @c full.
        int    window_ep{ 120 };      ///< Epochs after the return in which a hold of the satellite is tested (120 = 1 h at 30 s).
    };

    /// @brief The kind of an event.
    enum class EventKind {
        Open,       ///< A (satellite, code signal) lost its code bias.
        Close,      ///< Its bias is back; the satellite's re-validation window opens.
        Validated,  ///< A hold of the satellite agreed with the returned code.
        Released,   ///< A hold of the satellite disagreed by more than half a cycle: re-floated.
        Expired     ///< The window ended without a complete test (no hold, or holds that broke).
    };

    /// @brief One event, for the log, the satellite trace and the tests.
    struct Event {
        EventKind   kind{ EventKind::Open };  ///< What happened.
        std::string sat;                      ///< The satellite ("E18").
        std::string signal;                   ///< The code signal for Open and Close ("C5X"); empty otherwise.
        double      mean_cyc{ 0.0 };          ///< Validated / Released: mean offset of the held wide lane from the code (cycles).
        int         n{ 0 };                   ///< Validated / Released: epochs behind the mean; Close: epochs the gap lasted.
    };

    /// @brief Run counters; they only increase.
    struct Counters {
        uint64_t opened{ 0 };            ///< Open edges.
        uint64_t closed{ 0 };            ///< Close edges.
        uint64_t gap_epochs{ 0 };        ///< (satellite, code signal, epoch) triples inside a gap.
        uint64_t rows_excluded{ 0 };     ///< Code rows left out (full).
        uint64_t pivot_kept{ 0 };        ///< Code rows of the BSSD pivot kept in (its row stands in every single difference).
        uint64_t barred_epochs{ 0 };     ///< (satellite, epoch) pairs barred from a new fix (full).
        uint64_t windows{ 0 };           ///< Re-validation windows opened.
        uint64_t tested_epochs{ 0 };     ///< Held epochs tested inside a window.
        uint64_t validated{ 0 };         ///< Holds validated.
        uint64_t released{ 0 };          ///< Holds released.
        uint64_t expired{ 0 };           ///< Windows ended without a verdict.
    };

    /// @brief Half a wide-lane cycle: the rounding boundary of the tested quantity.
    static constexpr double kReleaseCycles = 0.5;

    /**
     * @brief Resolve the knobs; non-positive lengths fall back to the defaults.
     * @param c The configuration to copy.
     */
    void configure(const Config& c);

    /// @brief The knobs in force.  @return The configuration last passed to @c configure.
    const Config& config() const { return cfg_; }

    /// @brief True unless the mode is @c Off.  @return Whether the response is armed.
    bool armed() const { return cfg_.mode != Mode::Off; }

    /**
     * @brief Record whether one (satellite, code signal) of this epoch has its code bias.
     *
     * Call once per code signal of each modelled satellite whose stream record is present this
     * epoch; a signal not reported in an epoch keeps its state.  An Open or Close event is
     * appended to @c events() on an edge.
     *
     * @param sat      Satellite id ("E18").
     * @param signal   Code signal ("C5X").
     * @param has_bias True when the stream's fresh record holds a bias for the signal.
     */
    void observe(const std::string& sat, const std::string& signal, bool has_bias);

    /**
     * @brief True while a gap of this (satellite, code signal) is open.
     * @param sat Satellite id.  @param signal Code signal.
     * @return Whether the code row is without its bias.
     */
    bool inGap(const std::string& sat, const std::string& signal) const;

    /**
     * @brief True while any code signal of @p sat is in a gap.
     * @param sat Satellite id.  @return Whether the satellite has an open gap.
     */
    bool satInGap(const std::string& sat) const;

    /**
     * @brief The code rows to leave out of the update this epoch (mode @c full only).
     * @return Keys "<sat>_<signal>" of every open gap; empty in other modes.
     */
    std::vector<std::string> excludedRowKeys() const;

    /**
     * @brief True when a new fix of @p sat is barred this epoch (mode @c full only): a gap is
     *        open, or the satellite's window is in its first @c revalidate_ep epochs.
     * @param sat Satellite id.  @return Whether the satellite's unheld ambiguities are barred.
     */
    bool barred(const std::string& sat) const;

    /**
     * @brief Satellites whose re-validation window is open.
     * @return The satellites to sample this epoch.
     */
    std::vector<std::string> windowSatellites() const;

    /**
     * @brief Feed one epoch's sample of a satellite in its window and advance the window.
     *
     * @param sat        Satellite id with an open window.
     * @param offset_cyc The held wide lane minus the code's (median-removed), cycles; nullopt when
     *                   the satellite is not held this epoch or its code is not clean.
     * @return @c Released when the test failed (the engine re-floats the satellite), @c Validated
     *         when it passed, @c Expired when the window ended without a verdict; nullopt while
     *         the window is still running.  The event is also appended to @c events().
     *
     * A run of samples is broken by an epoch without one; the window restarts its count from the
     * next sample.  A verdict closes the window; a release opens a new window at once, so the
     * re-floated satellite is barred (in @c full) and tested again.
     */
    std::optional<EventKind> sample(const std::string& sat, std::optional<double> offset_cyc);

    /// @brief Record that @p n code rows were left out this epoch.  @param n Rows left out.
    void noteRowsExcluded(uint64_t n) { ctr_.rows_excluded += n; }
    /// @brief Record that @p n code rows of the pivot were kept in.  @param n Rows kept.
    void notePivotKept(uint64_t n) { ctr_.pivot_kept += n; }
    /// @brief Record that @p n satellites were barred this epoch.  @param n Satellites barred.
    void noteBarred(uint64_t n) { ctr_.barred_epochs += n; }

    /// @brief The events since the last @c clearEvents.  @return The event list.
    const std::vector<Event>& events() const { return events_; }
    /// @brief Forget this epoch's events (the engine calls it after logging them).
    void clearEvents() { events_.clear(); }

    /// @brief The run counters.  @return Const reference to the counters.
    const Counters& counters() const { return ctr_; }

    /// @brief Counter closure: every closed window ended in exactly one verdict or an expiry.
    /// @return True when validated + released + expired + open windows == windows.
    bool closureHolds() const;

    /**
     * @brief Offsets of held wide lanes from the code, median-removed per system.
     *
     * @param d Per satellite: held wide lane minus the corrected Melbourne-Wubbena (cycles), for
     *          the held satellites with clean code this epoch.
     * @return Per satellite: d minus the median of d over the satellites of its system (first
     *         character of the id); a system with fewer than four satellites gives no offsets.
     */
    static std::map<std::string, double> medianRemoved(const std::map<std::string, double>& d);

    /**
     * @brief The corrected Melbourne-Wubbena of one satellite, in wide-lane cycles.
     *
     * @param f1,f2   Carrier frequencies (Hz) of the two bands.
     * @param phi1_m,phi2_m  Carrier phases (m).
     * @param p1_m,p2_m      Codes (m).
     * @param pb1_m,pb2_m    The stream's satellite phase biases (m); observation = ... + bias.
     * @param cb1_m,cb2_m    The stream's satellite code biases (m).
     * @return The wide-lane ambiguity the code gives, plus the receiver's wide-lane bias (cycles).
     */
    static double correctedMelbourneWubbena(double f1, double f2, double phi1_m, double phi2_m,
                                            double p1_m, double p2_m, double pb1_m, double pb2_m,
                                            double cb1_m, double cb2_m);

private:
    struct Window {
        int    age{ 0 };       ///< Epochs since the window opened.
        int    run{ 0 };       ///< Consecutive held samples.
        double sum{ 0.0 };     ///< Their sum (cycles).
    };

    Config cfg_;
    Counters ctr_;
    std::map<std::string, int> open_;          ///< "<sat>|<signal>" -> epochs open so far.
    std::map<std::string, Window> windows_;    ///< sat -> its re-validation window.
    std::vector<Event> events_;
};

/// @brief Parse the mode's key value ("off", "revalidate", "full"); anything else is Off.
/// @param s Key value.  @return The mode.
CodeBiasGap::Mode codeBiasGapModeFromString(const std::string& s);

/// @brief The mode's key value.  @param m Mode.  @return "off", "revalidate" or "full".
const char* codeBiasGapModeName(CodeBiasGap::Mode m);

/// @brief The event's name on the log and the consumer satellite trace.
/// @param k Event kind.  @return "cbgap_open", "cbgap_close", "cbgap_validated", "cbgap_released", "cbgap_expired".
const char* codeBiasGapEventName(CodeBiasGap::EventKind k);

}  // namespace GnssProcessor
