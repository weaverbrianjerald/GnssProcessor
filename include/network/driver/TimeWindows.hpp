#pragma once
/**
 * @file TimeWindows.hpp
 * @brief The network producer's table of time windows: every live window that is counted in
 *        epochs, or that bounds a gap in seconds, owned in one place, in seconds, with the clock
 *        that counts it.
 *
 * ### What it answers
 *
 * A window of the network producer is a duration a decision waits for, or a gap beyond which a
 * carried quantity is no longer trusted.  Eighteen live windows were written in three units: some
 * as seconds (a gap bound such as the receiver code-bias horizon), some as a count of epochs (the
 * ring fix's persistence count), one as a count per epoch (the ring fix's decision budget).  At the
 * one sampling interval every run has used so far (30 s) the three units are interchangeable; at
 * any other interval a count of epochs spans a different number of seconds.  This file states each
 * window once, in seconds at a fixed reference interval, together with
 *
 *  - the unit the code counts it in today (@ref GnssProcessor::NetworkWindowUnit "NetworkWindowUnit"),
 *  - the clock that counts it (@ref GnssProcessor::NetworkWindowClock "NetworkWindowClock"): the epochs in which the station,
 *    arc or signal was observed (presence), or the solves that make the decision (decision),
 *  - the configuration key that sets it, if any, and its loader default,
 *  - the value the two reference configurations arm, in seconds,
 *  - the source location whose literal it replaces.
 *
 * ### How a window resolves, and why no run changes
 *
 * An epoch-counted window resolves to its count as its seconds divided by
 * @ref GnssProcessor::kNetworkReferenceIntervalS "kNetworkReferenceIntervalS" (30 s), a constant of this file and NOT the run's sampling
 * interval.  So every window resolves to the integer or value it had before the table existed, at
 * every sampling rate, and the output of every run is unchanged.  A variable-rate design replaces
 * the reference interval, row by row, by the spacing of the row's counting clock; that step changes
 * output and is not taken here.
 *
 * Two kinds of row:
 *
 *  - **Constant rows** (no configuration key).  The literal lives only in this table: the named
 *    constant at its old location (for example @c kRingFixGapS in network/ar/RingFix.hpp) is
 *    defined from the row through @ref GnssProcessor::networkTimeWindowSeconds "networkTimeWindowSeconds" or @ref GnssProcessor::networkTimeWindowEpochs "networkTimeWindowEpochs",
 *    so every call site keeps its name and its value.
 *  - **Configuration rows.**  The key, its loader default and the value the two reference
 *    configurations arm are recorded here; the code keeps reading the loaded value.  The keys keep
 *    their JSON names and loader defaults: a rename would invalidate configurations and snapshot
 *    fingerprints.  The unit test of the table pins each recorded default against the loader and
 *    each recorded reference value against the two reference configurations.
 *
 * ### The run-start table
 *
 * @ref GnssProcessor::networkTimeWindowsTable "networkTimeWindowsTable" renders the rows with the value in force on the run;
 * @ref GnssProcessor::networkTimeWindowsLog "networkTimeWindowsLog" writes it once to the text log at setup, under the prefix
 * "Network (time windows as armed)".  It writes no trace file and no product.
 *
 * ### Adding a window
 *
 * A new window of the producer that is counted in epochs or bounds a gap gets a value of
 * @ref GnssProcessor::NetworkTimeWindow "NetworkTimeWindow" (before @c Count), its row in @ref GnssProcessor::kNetworkTimeWindows "kNetworkTimeWindows" at the same position,
 * and, for a configuration row, its case in @ref GnssProcessor::networkTimeWindowInForce "networkTimeWindowInForce".  A constant reads
 * its value from the row; it is never written as a literal at the call site.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkRun;

/// @brief The reference interval (s) at which an epoch-counted window's seconds resolve to its
///        count of epochs: the sampling interval every window's count was written for.
///
/// A fixed constant, not the run's sampling interval, so that the count of every window is the
/// same at every rate.  A variable-rate design replaces it, per window, by the spacing of the
/// window's counting clock.
inline constexpr double kNetworkReferenceIntervalS = 30.0;

/**
 * @brief Identity of one time window of the network producer; the value is the window's row
 *        index in @ref kNetworkTimeWindows.
 */
enum class NetworkTimeWindow : std::uint8_t {
    RingFixGap,            ///< A pause between two chord epochs of a link that restarts its continuity count.
    ClockAnchorMaxGap,     ///< The clock anchor's per-satellite staleness bound.
    RingFixMaxDecisions,   ///< The ring fix's budget of new decisions per epoch.
    RingFixPersist,        ///< The ring fix's persistence count: passes with the same integers before a decision.
    RingFixMonitor,        ///< Consecutive epochs above the residual monitor's bound that release a held link.
    RingFixSettledWait,    ///< Emissions a held family waits before its settled flags are raised.
    RingFixNnGuardMin,     ///< The near-null guard's minimum run before it may act.
    ArSuspect,             ///< The producer AR's suspect window after an event.
    PhaseLevelingWarmup,   ///< Emitted epochs before a carried ambiguity is trusted.
    ClockAnchorRecouple,   ///< Consecutive in-band epochs before a decoupled satellite's anchor re-engages.
    ReadmitMinGap,         ///< The re-admission detector's minimum absence.
    PhaseLevelingMaxGap,   ///< The gap a carried ambiguity survives before it is dropped.
    RxCbMaxGap,            ///< The gap horizon of the receiver code-bias carry.
    PhaseColourMaxGap,     ///< The gap the carried phase colour survives.
    ReadmitVerify,         ///< The window after a clock seed in which an anchor trip is attributed to the seed.
    RingFixWindow,         ///< The ring fix's event window: no decision on a link with an event inside it.
    ClockEmitRebaseGap,    ///< The emitted-clock re-base's key gap beyond which a return is a first sighting.
    ReadmitSeedHorizon,    ///< The horizon within which a departed satellite's parked clock seeds its return.
    Count                  ///< Number of windows (not a window).
};

/// @brief Number of windows in @ref kNetworkTimeWindows.
inline constexpr std::size_t kNetworkTimeWindowCount = static_cast<std::size_t>(NetworkTimeWindow::Count);

/// @brief The unit in which the code counts a window today.
enum class NetworkWindowUnit : std::uint8_t {
    Seconds,    ///< A duration in seconds (read as is).
    Epochs,     ///< A count of epochs (seconds / @ref kNetworkReferenceIntervalS).
    PerEpoch,   ///< A count per epoch (a rate per second times @ref kNetworkReferenceIntervalS).
};

/// @brief The clock that counts a window.
enum class NetworkWindowClock : std::uint8_t {
    Presence,   ///< Seconds of epochs in which the station, arc or signal was observed.
    Decision,   ///< Seconds between the solves that make the decision.
};

/**
 * @brief One row of the table of time windows.
 *
 * The seconds fields hold a duration in seconds for a window in seconds or in epochs; for the
 * one window counted per epoch they hold a rate per second, so that the count per epoch at the
 * reference interval is the rate times @ref kNetworkReferenceIntervalS.
 */
struct NetworkTimeWindowRow {
    NetworkTimeWindow  id;            ///< The window's identity (equals its row index).
    const char*        name;          ///< Short name, as printed in the run-start table.
    const char*        config_key;    ///< The JSON key that sets it ("" for a constant).
    NetworkWindowUnit  unit;          ///< The unit the code counts it in today.
    NetworkWindowClock clock;         ///< The clock that counts it.
    double             default_s;     ///< The constant, or the key's loader default, in seconds (per second for @c PerEpoch) at the reference interval.
    double             reference_s;   ///< The value the two reference configurations arm, in seconds (per second for @c PerEpoch) at the reference interval.
    const char*        replaces;      ///< The source location whose literal (constant) or loader default (key) the row stands for.
};

/**
 * @brief The table of time windows, one row per @ref NetworkTimeWindow, in identity order.
 *
 * Values in seconds at the reference interval of 30 s.  "presence" counts the epochs in which the
 * thing was observed; "decision" counts the solves that make the decision.
 */
inline constexpr std::array<NetworkTimeWindowRow, kNetworkTimeWindowCount> kNetworkTimeWindows{{
    {NetworkTimeWindow::RingFixGap, "ring fix chord gap", "",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Decision, 45.0, 45.0,
     "include/network/ar/RingFix.hpp kRingFixGapS"},
    {NetworkTimeWindow::ClockAnchorMaxGap, "clock anchor staleness", "network.clock_anchor_max_gap_s",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Decision, 120.0, 120.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::RingFixMaxDecisions, "ring fix decisions per epoch", "network.ring_fix.max_decisions_per_epoch",
     NetworkWindowUnit::PerEpoch, NetworkWindowClock::Decision, 0.1, 0.4,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::RingFixPersist, "ring fix persistence", "",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Decision, 300.0, 300.0,
     "include/network/ar/RingFix.hpp kRingFixPersistEpochs"},
    {NetworkTimeWindow::RingFixMonitor, "ring fix residual monitor", "",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Presence, 120.0, 120.0,
     "include/network/ar/RingFix.hpp kRingFixMonitorEpochs"},
    {NetworkTimeWindow::RingFixSettledWait, "ring fix settled wait", "network.ring_fix.settled_wait_emissions",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Decision, 0.0, 300.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::RingFixNnGuardMin, "ring fix near-null guard run", "network.ring_fix.nn_guard_min_epochs",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Decision, 1800.0, 1800.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::ArSuspect, "producer AR suspect window", "network.ambiguity_resolution.suspect_epochs",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Decision, 0.0, 300.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::PhaseLevelingWarmup, "phase leveling warm-up", "network.phase_leveling_warmup_epochs",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Presence, 180.0, 180.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::ClockAnchorRecouple, "clock anchor re-coupling", "network.clock_anchor_recouple_epochs",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Decision, 120.0, 120.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::ReadmitMinGap, "re-admission minimum absence", "",
     NetworkWindowUnit::Epochs, NetworkWindowClock::Presence, 90.0, 90.0,
     "include/network/driver/RunTypes.hpp kReadmitMinGapEp"},
    {NetworkTimeWindow::PhaseLevelingMaxGap, "phase leveling carry gap", "network.phase_leveling_max_gap_s",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 120.0, 900.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::RxCbMaxGap, "receiver code-bias carry gap", "",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 900.0, 900.0,
     "include/network/driver/RunTypes.hpp kRxCbMaxGapS"},
    {NetworkTimeWindow::PhaseColourMaxGap, "phase colour carry gap", "network.phase_colour_max_gap_s",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 900.0, 900.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
    {NetworkTimeWindow::ReadmitVerify, "re-admission seed verification", "",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 300.0, 300.0,
     "include/network/driver/RunTypes.hpp kReadmitVerifyWindowS"},
    {NetworkTimeWindow::RingFixWindow, "ring fix event window", "",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Decision, 600.0, 600.0,
     "include/network/ar/RingFix.hpp kRingFixWindowS"},
    {NetworkTimeWindow::ClockEmitRebaseGap, "emitted clock re-base gap", "",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 600.0, 600.0,
     "include/network/emit/ClockEmitRebase.hpp NetworkClockEmitRebase::Config::max_gap_s"},
    {NetworkTimeWindow::ReadmitSeedHorizon, "re-admission seed horizon", "network.readmit_seed_horizon_s",
     NetworkWindowUnit::Seconds, NetworkWindowClock::Presence, 0.0, 3900.0,
     "src/core/config/GnssConfigLoadNetwork.cpp loader default"},
}};

/**
 * @brief The row of a window.
 * @param w The window (not @c Count).
 * @return Its row in @ref kNetworkTimeWindows.
 */
constexpr const NetworkTimeWindowRow& networkTimeWindowRow(NetworkTimeWindow w) {
    return kNetworkTimeWindows[static_cast<std::size_t>(w)];
}

/**
 * @brief A non-negative value rounded to the nearest whole number (constexpr; @c std::lround is
 *        not constexpr in C++17).
 * @param x The value (>= 0).
 * @return The nearest whole number.
 */
constexpr long long networkTimeWindowRound(double x) {
    return static_cast<long long>(x + 0.5);
}

/**
 * @brief A value in seconds (or per second) expressed in a unit at the reference interval.
 * @param unit      The unit to express it in.
 * @param seconds   The value in seconds (per second for @c PerEpoch).
 * @return The value in @p unit: seconds as is, epochs as seconds / @ref kNetworkReferenceIntervalS,
 *         a count per epoch as the rate times @ref kNetworkReferenceIntervalS.
 */
constexpr double networkTimeWindowInUnit(NetworkWindowUnit unit, double seconds) {
    return unit == NetworkWindowUnit::Epochs     ? seconds / kNetworkReferenceIntervalS
         : unit == NetworkWindowUnit::PerEpoch   ? seconds * kNetworkReferenceIntervalS
                                                 : seconds;
}

/**
 * @brief A window's default (the constant, or the key's loader default) in seconds at the
 *        reference interval.
 * @param w The window.
 * @return @ref NetworkTimeWindowRow::default_s of its row (per second for a @c PerEpoch window).
 */
constexpr double networkTimeWindowSeconds(NetworkTimeWindow w) {
    return networkTimeWindowRow(w).default_s;
}

/**
 * @brief A window's default in the unit the code counts it in, at the reference interval.
 * @param w The window.
 * @return Seconds for a @c Seconds window, a count of epochs for an @c Epochs window, a count
 *         per epoch for a @c PerEpoch window.
 */
constexpr double networkTimeWindowDefault(NetworkTimeWindow w) {
    return networkTimeWindowInUnit(networkTimeWindowRow(w).unit, networkTimeWindowRow(w).default_s);
}

/**
 * @brief A count-valued window's default as a whole count at the reference interval (epochs, or
 *        a count per epoch).
 * @param w The window (unit @c Epochs or @c PerEpoch).
 * @return @ref networkTimeWindowDefault rounded to the nearest whole number.  Every such row
 *         resolves exactly (checked at compile time by @ref networkTimeWindowsWellFormed).
 */
constexpr long long networkTimeWindowEpochs(NetworkTimeWindow w) {
    return networkTimeWindowRound(networkTimeWindowDefault(w));
}

/**
 * @brief Whether a value in a unit is exact: a count (epochs or per epoch) must be a whole number.
 * @param unit  The unit.
 * @param value The value in that unit.
 * @return True for a @c Seconds value, or a whole count.
 */
constexpr bool networkTimeWindowExact(NetworkWindowUnit unit, double value) {
    return unit == NetworkWindowUnit::Seconds
        || static_cast<double>(networkTimeWindowRound(value)) == value;
}

/**
 * @brief The compile-time checks of the table: each row's identity equals its index, and each
 *        count-valued row's default and reference value resolve to whole counts at the reference
 *        interval.
 * @return True when every check holds.
 */
constexpr bool networkTimeWindowsWellFormed() {
    for (std::size_t i = 0; i < kNetworkTimeWindowCount; ++i) {
        const NetworkTimeWindowRow& r = kNetworkTimeWindows[i];
        if (static_cast<std::size_t>(r.id) != i) return false;
        if (!networkTimeWindowExact(r.unit, networkTimeWindowInUnit(r.unit, r.default_s))) return false;
        if (!networkTimeWindowExact(r.unit, networkTimeWindowInUnit(r.unit, r.reference_s))) return false;
    }
    return true;
}

static_assert(networkTimeWindowsWellFormed(),
              "kNetworkTimeWindows: a row is out of identity order or a count does not resolve to a whole "
              "number at the reference interval");

/**
 * @brief The value of a window in force on a run, in the unit the code counts it in.
 *
 * A constant row returns its default; a configuration row returns the loaded value of its key,
 * read through the same getter or member the code reads.  Display only: no code path reads a
 * window through this function.
 *
 * @param w      The window.
 * @param config The loaded configuration.
 * @return The value in force, in the window's unit.
 */
double networkTimeWindowInForce(NetworkTimeWindow w, const GnssConfig& config);

/**
 * @brief The run-start table of the time windows, one line per window after a header.
 *
 * Each line names the window, its counting clock and unit, the value in force (in its unit),
 * that value in seconds at the reference interval, and its key (or "constant").  A line ends in
 * "(default ...)" when a key is set away from its loader default.
 *
 * @param config The loaded configuration.
 * @return The lines, without a prefix.
 */
std::vector<std::string> networkTimeWindowsTable(const GnssConfig& config);

/**
 * @brief Setup step: write @ref networkTimeWindowsTable once to the text log under the prefix
 *        "Network (time windows as armed)".  Writes no trace file, manifest record or product.
 * @param run The run (reads @c config and @c log).
 */
void networkTimeWindowsLog(const NetworkRun& run);

}  // namespace GnssProcessor
