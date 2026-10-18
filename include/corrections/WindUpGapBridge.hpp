#pragma once

/**
 * @file WindUpGapBridge.hpp
 * @brief Carrying the accumulated phase wind-up angle across a data gap.
 *
 * The wind-up accumulator (GnssCorrections::computePhaseWindUp) unwraps each epoch
 * against the previous one, which is unambiguous only while consecutive epochs are
 * less than half a cycle apart.  After a gap of @c estimation.phase_windup_max_gap_s
 * (300 s by default) it restarts from the raw @c atan2 angle, which applies an
 * exact integer-cycle step to every signal of the satellite.
 *
 * This module is the alternative for a gap that is still inside the horizon of the
 * leveling carry: walk the angle across the gap through virtual epochs, at most
 * @c Options::step_s apart, evaluated from the satellite orbit (the SP3 geometry)
 * with the receiver where it is now, unwrapping every step.  The raw angle of the
 * current epoch then supplies the fractional part, and the walk supplies only the
 * integer number of cycles, so an error of the virtual geometry matters only if it
 * exceeds a quarter of a cycle.
 *
 * The walk gives up, and the caller restarts as it always did, when
 *  - a virtual epoch has no satellite state or a degenerate geometry,
 *  - the satellite is in the Earth's umbra at a virtual epoch (the real yaw
 *    differs from the nominal yaw the model uses),
 *  - the satellite is in a noon-turn window (SatAttitude::inNoonTurnWindow),
 *  - the angle moves by a quarter of a cycle or more over half a step, which the
 *    unwrap cannot be trusted to resolve (a pass near the receiver's zenith, or a
 *    nominal yaw near its singularity at low beta), or
 *  - the walk ends more than a quarter of a cycle from the raw angle of the
 *    current epoch, which means the virtual geometry and the epoch geometry
 *    disagree.
 *
 * The functions here are pure: the geometry arrives through a sampler, so the
 * arithmetic is tested without orbits.
 */

#include <cstddef>
#include <functional>
#include <string>

namespace GnssProcessor {
namespace WindUpBridge {

/// Largest change of the angle over one virtual step the unwrap accepts, in cycles.
/// Each half step (the walk resolves a step at its middle) must stay below half of it.
inline constexpr double kMaxStepCycles = 0.5;

/// Most attempts one receiver keeps in its event list (the counters count them all).
inline constexpr std::size_t kMaxEvents = 512;

/// Largest disagreement between the end of the walk and the raw angle of the
/// current epoch, in cycles; beyond it the integer cycle count is not trusted.
inline constexpr double kClosureCycles = 0.25;

/**
 * @brief Settings of the gap bridge.
 */
struct Options {
    double max_gap_s{900.0};   ///< Longest gap bridged (s); a longer one restarts as before.
    double step_s{30.0};       ///< Largest spacing of the virtual epochs (s).
    double shadow_scale{1.0};  ///< Earth-radius multiplier of the umbra test (processing.eclipse_shadow_scale).
};

/**
 * @brief How a bridging attempt ended.
 */
enum class Outcome {
    Bridged,       ///< The walk completed and its integer cycle count was accepted.
    NoState,       ///< A virtual epoch had no satellite state, or a degenerate geometry.
    Umbra,         ///< The satellite was in the Earth's umbra at a virtual epoch.
    NoonTurn,      ///< The satellite was in a noon-turn window at a virtual epoch.
    StepTooLarge,  ///< The angle moved by half a cycle or more over one step.
    Closure        ///< The walk ended far from the raw angle of the current epoch.
};

/**
 * @brief One virtual epoch of the walk, as the caller's sampler reports it.
 */
struct Sample {
    bool   ok{false};          ///< False when the epoch has no usable state; the other members are then unread.
    bool   umbra{false};       ///< The satellite is in the Earth's umbra.
    bool   noon_turn{false};   ///< The satellite is in a noon-turn window.
    double angle_rad{0.0};     ///< Raw (wrapped) wind-up angle at the epoch (rad).
};

/// Source of the geometry at a virtual epoch; the argument is continuous GPS seconds.
using Sampler = std::function<Sample(double t_s)>;

/**
 * @brief Result of one bridging attempt.
 */
struct Result {
    Outcome   outcome{Outcome::NoState};  ///< How the attempt ended.
    double    angle_rad{0.0};             ///< Accumulated angle at the current epoch (rad); set only when Bridged.
    long long cycles{0};                  ///< Whole cycles by which the accepted angle exceeds the raw one; set only when Bridged.  Nonzero means a restart would have stepped the model by minus this many cycles.
    int       samples{0};                 ///< Virtual epochs evaluated, half steps included.
};

/**
 * @brief One bridging attempt, named: the identity behind the counters.
 */
struct Event {
    std::string satellite;                ///< Satellite identifier.
    double      t_now_s{0.0};             ///< Continuous GPS seconds of the epoch that closed the gap.
    double      gap_s{0.0};               ///< Length of the gap (s).
    Outcome     outcome{Outcome::NoState};///< How the attempt ended.
    long long   cycles{0};                ///< Whole cycles by which the bridged angle exceeds the raw one (Bridged only); a restart would have stepped the model by minus this.
};

/**
 * @brief Counters of the bridge, per receiver.
 *
 * The ON-path telemetry of the option: an armed run that counts nothing here
 * while the census counts gaps in the bridged range has dead wiring.
 */
struct Counters {
    long long attempts{0};          ///< Gaps inside the bridged range, bridged or not.
    long long bridged{0};           ///< Gaps carried across by the walk (no restart, no ledger entry).
    long long bridged_stepped{0};   ///< Bridged gaps whose integer cycle count was not zero.
    long long refused_no_state{0};  ///< Outcome::NoState, including a missing satellite-state provider.
    long long refused_umbra{0};     ///< Outcome::Umbra.
    long long refused_noon_turn{0}; ///< Outcome::NoonTurn.
    long long refused_step{0};      ///< Outcome::StepTooLarge.
    long long refused_closure{0};   ///< Outcome::Closure.
    long long beyond_horizon{0};    ///< Gaps at or beyond Options::max_gap_s: restarted as before, not attempted.
    long long samples{0};           ///< Virtual epochs evaluated, over all attempts.

    /// @brief Gaps the walk refused, over all reasons.
    long long refused() const {
        return refused_no_state + refused_umbra + refused_noon_turn + refused_step + refused_closure;
    }

    /**
     * @brief Record one attempt.
     * @param r The attempt's result.
     */
    void record(const Result& r);

    /**
     * @brief Add another receiver's counters (the roster sum of the run-end line).
     * @param o Counters to add.
     */
    void add(const Counters& o);
};

/**
 * @brief Short name of an outcome, for logs.
 * @param o Outcome.
 * @return A static string.
 */
const char* outcomeName(Outcome o);

/**
 * @brief Walk the accumulated wind-up angle across a gap.
 *
 * The gap is divided into equal steps of at most @c Options::step_s.  Each step is
 * evaluated at its middle too, and the step is the sum of its two half steps, each
 * wrapped to half a cycle either way.  A half step of half of @c kMaxStepCycles or
 * more (a quarter cycle) refuses the walk: a wrapped half step cannot be told from
 * its alias, and the margin makes a true half step of up to three quarters of a
 * cycle show.  The sum of
 * all steps is added to @p angle_prev_rad, and the integer number of cycles between
 * that angle and @p angle_now_rad is then taken, so the returned angle is
 * @p angle_now_rad plus a whole number of cycles.  The walk is refused when it
 * ends more than @c kClosureCycles from @p angle_now_rad.
 *
 * @param[in] t_prev_s        Continuous GPS seconds of the last accumulated epoch.
 * @param[in] t_now_s         Continuous GPS seconds of the current epoch (after @p t_prev_s).
 * @param[in] angle_prev_rad  Accumulated angle at @p t_prev_s (rad).
 * @param[in] angle_now_rad   Raw wrapped angle at @p t_now_s from the epoch's own geometry (rad).
 * @param[in] opt             Bridge settings; only @c step_s is read here.
 * @param[in] sampler         Geometry of a virtual epoch.
 * @return The attempt's result.
 */
Result walk(double t_prev_s, double t_now_s,
            double angle_prev_rad, double angle_now_rad,
            const Options& opt, const Sampler& sampler);

}  // namespace WindUpBridge
}  // namespace GnssProcessor
