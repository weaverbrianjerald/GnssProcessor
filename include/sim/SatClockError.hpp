#pragma once

/**
 * @file SatClockError.hpp
 * @brief Synthetic satellite-clock error, injected on top of the precise CLK product.
 *
 * ## Purpose
 *
 * The simulator injects the precise CLK product as the satellite clock, and the
 * network producer emits its clock as a correction to that same resolved product
 * (the S line's @c apriori_id names the resolved SP3 and CLK file basenames, set in
 * `ApplicationNetworkSetup.cpp`).  The a-priori clock and the truth are therefore the
 * same object, with two consequences:
 *
 *  - with no injected error, the estimable satellite-clock truth is identically zero:
 *    a valid null test, but not a measurement; and
 *  - no clock event can be exercised, so the Q3 temporal-anchor guard has no
 *    population to be graded against.
 *
 * This class supplies both: a stochastic background (per-satellite offset, rate
 * and random walk) and deterministic scripted steps at named epochs.
 *
 * ## Determinism contract
 *
 * A synthetic network is synthesized as N independent processes, one per station
 * (see docs/SPEC_synthetic_minimum_network.md §1).  Every satellite-side quantity
 * must therefore be a pure function of (satellite, time) that reproduces
 * bit-for-bit across processes.  Two properties provide that:
 *
 *  1. The series is **pre-generated on the epoch grid**, never drawn lazily inside
 *     the satellite loop.  A lazy draw would also depend on the sky, because
 *     @c forwardPseudorange is called before the elevation-mask test: the draw
 *     count would change with visibility and break the storm-on/off byte-identity
 *     that the simulator's RNG-stream separation provides.
 *  2. Each satellite's stream is seeded from @c hash(seed, satellite id), not from
 *     a shared sequence, so a satellite's realization does not depend on which
 *     other satellites are present or on their iteration order.
 *
 * Off-grid evaluation (the emission time is ~70 ms before the epoch, and differs
 * per station with the geometry) is a linear interpolation of that fixed table,
 * and so is still a pure function of (satellite, time).
 *
 * ## Injection site
 *
 * The error goes into the forward model's clock lookup and nowhere else.  Adding it
 * inside @c EphemerisProvider::clock() would be a one-line change and is wrong: that
 * entry point also feeds the internal SPP and the Part-B answer key, so the 1 mm
 * noise-free self-consistency gate would move with the injection and pass however
 * wrong the arc had become.
 *
 * The caller must also keep two invariants:
 *
 *  - **Apply the error after the clock-coverage test.**  The forward model
 *    overloads @c |sat_clk| @c < @c 1e-12 as "no clock covers this epoch, drop the
 *    satellite".  Perturbing the value before that predicate can fabricate a clock
 *    for an uncovered satellite, or cancel a real one and drop a healthy satellite.
 *  - **Leave the noise-free SPP self-check on the unperturbed range.**  The 1 mm
 *    gate verifies the forward model, not the injected fault; feeding it the fault
 *    makes it report an injection bug at metre level.  The caller adds @c metres()
 *    back when building the clean SPP rows, so that gate keeps its exact meaning
 *    while the emitted RINEX carries the fault.
 *
 * ## Sign and unit convention (the grader depends on this)
 *
 * @c metres() returns the satellite clock error expressed as **metres of clock
 * offset**, i.e. `c · (dt_true − dt_product)`.  The injected clock is
 * `dt_true = dt_product + metres()/c`, so the emitted pseudorange
 * `P = range + c·dt_r − c·dt_true` is smaller than the unperturbed one by exactly
 * @c metres().  Truth is published with the same sign in @c truth_satclock.csv.
 */

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "math/TimeUtils.hpp"  // GnssTime
#include "sim/SimConfig.hpp"

namespace GnssProcessor {
namespace sim {

/**
 * @brief A pre-generated, per-satellite synthetic clock-error series.
 *
 * Default-constructed and not built, the object is inactive and @c metres() returns
 * a literal 0.0 for every query, so an unarmed run is byte-identical.
 */
class SatClockError {
public:
    /**
     * @brief Pre-generate the per-satellite series on the epoch grid.
     *
     * @param cfg         The @c sat_clock knob block.  All-zero amplitudes with an
     *                    empty step list leave the object inactive.
     * @param sats        Satellite ids to generate for (e.g. every PRN in the plan).
     *                    A satellite absent here but named by a scripted step is an
     *                    error: a step that injects nothing would be graded as an
     *                    undetected event.
     * @param t0          First epoch of the arc.
     * @param interval_s  Epoch spacing (s); must be > 0.
     * @param n_epochs    Number of epochs; must be > 0.
     * @param err         Optional: set to a human-readable reason on failure.
     * @return true on success (including the inactive case), false with @p err set.
     */
    bool build(const SimConfig::SatClock& cfg,
               const std::vector<std::string>& sats,
               const GnssTime& t0, double interval_s, int n_epochs,
               std::string* err = nullptr);

    /** @brief True when any error channel is armed (otherwise @c metres() is 0.0). */
    bool active() const { return active_; }

    /**
     * @brief The injected clock error at @p t for @p sat, in metres of clock offset.
     *
     * Linear interpolation of the pre-generated grid, clamped at both ends (the
     * emission time of the first epoch lies slightly before @c t0).  Returns 0.0
     * for an unknown satellite or an inactive object.
     */
    double metres(const std::string& sat, const GnssTime& t) const;

    /**
     * @brief Write @c truth_satclock.csv: one row per (epoch, satellite).
     *
     * Columns: `week,tow_s,satellite,clk_err_m,clk_err_s`.  Written only when the
     * object is active: an absent file states "this channel was not simulated",
     * which an empty one cannot (standing invariant #4: blank is never 0).
     */
    bool writeTruth(const std::filesystem::path& path) const;

    /** @brief Largest |error| over the whole table (m); 0 when inactive. */
    double peakAbsMetres() const;

    /** @brief Number of satellites carrying a series. */
    std::size_t satelliteCount() const { return table_.size(); }

private:
    bool active_{ false };
    GnssTime t0_{};
    double interval_s_{ 30.0 };
    int n_epochs_{ 0 };
    std::map<std::string, std::vector<double>> table_; ///< satellite -> per-epoch metres
};

} // namespace sim
} // namespace GnssProcessor
