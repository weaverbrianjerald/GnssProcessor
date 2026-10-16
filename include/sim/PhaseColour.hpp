#pragma once

/**
 * @file PhaseColour.hpp
 * @brief Band-common, per-line-of-sight Gauss-Markov carrier-phase error.
 *
 * ## Purpose
 *
 * The simulator's historical phase error is the wrong shape for every
 * persistence-sensitive result.  Its ionosphere-free post-fit phase residual is
 * about 80 % white (14 mm) with a weak 5-minute multipath, where the field's is
 * about 90 % a first-order Gauss-Markov process (5 to 7 mm at the class median,
 * correlation time 80 to 120 s, strongest in the tropics) over about 2 mm of
 * white noise (docs/refound/SFR_sim_fixture_coloured_phase_2026-10-01.md,
 * section 1).  This class supplies the missing coloured component.
 *
 * ## The model
 *
 * One unit-variance first-order Gauss-Markov process @f$u@f$ per line of sight
 * (one per satellite at a station; each station is its own simulator process, so
 * the station dimension is the process and its salted seed):
 *
 *  - first visible epoch: @f$u = g@f$ (a stationary start, so the process has unit
 *    variance from its first sample and carries no ignition transient);
 *  - afterwards: @f$u = \phi\,u_{prev} + \sqrt{1-\phi^2}\,g@f$ with
 *    @f$\phi = e^{-\Delta t/\tau}@f$, where @f$\Delta t@f$ is the time since this
 *    line of sight's previous visible epoch, so a visibility gap decorrelates the
 *    process by exactly the amount the gap implies (a gap of many @f$\tau@f$ is
 *    indistinguishable from a fresh stationary start);
 *  - @f$g \sim N(0,1)@f$ from the class's own @c std::mt19937_64 stream.
 *
 * The injected error is @f$e = \sigma_{zen}\,f(el)\,u@f$ metres, with
 * @f$f(el) = 1/\sin(\max(el, el_{floor}))@f$, the elevation factor of the
 * simulator's white phase noise.
 *
 * ## Band-common by construction
 *
 * The caller adds the same metres @f$e@f$ to the carrier phase of every band of
 * the satellite (converted to cycles with each band's wavelength) and adds nothing
 * to code.  The ionosphere-free combination therefore keeps all of it (its
 * coefficients sum to one) and the geometry-free combination none of it, so the
 * estimator's free slant ionosphere cannot absorb it, which is the property the
 * field residual shows.  @ref PhaseColourGenerator::sample returns the stored
 * value without a new draw when called again for the same line of sight at the
 * same instant, so a per-band call site still injects one realization per
 * satellite and epoch.
 *
 * ## Byte-identity
 *
 * With @c sigma_zen_m @c == @c 0 (the default) the generator is inactive:
 * @ref PhaseColourGenerator::sample returns a literal @c 0.0 and draws nothing.
 * The stream is dedicated, so even an active block cannot shift the white-noise,
 * multipath/slip, troposphere or receiver-clock realizations.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <string>

#include "common/Constants.hpp"

namespace GnssProcessor {
namespace sim {

/**
 * @brief One step of a unit-variance first-order Gauss-Markov recursion.
 *
 * @f$u = \phi\,u_{prev} + \sqrt{1-\phi^2}\,g@f$, @f$\phi = e^{-\Delta t/\tau}@f$.
 * If @p u_prev has unit variance and @p g is an independent standard normal, the
 * result has unit variance and lag correlation @f$\phi@f$ with @p u_prev.
 *
 * @param u_prev  The process value at the previous sample (unit variance).
 * @param dt_s    Time since the previous sample (s); must be >= 0.
 * @param tau_s   Correlation time (s); must be > 0.
 * @param g       An independent standard-normal draw.
 * @return The process value at the new sample.
 */
inline double gaussMarkovUnitStep(double u_prev, double dt_s, double tau_s, double g) noexcept {
    const double phi = std::exp(-dt_s / tau_s);
    return phi * u_prev + std::sqrt(std::max(0.0, 1.0 - phi * phi)) * g;
}

/**
 * @brief Elevation amplification of the phase-colour error.
 *
 * The same form as the simulator's white phase noise:
 * @f$1/\sin(\max(el, el_{floor}))@f$, so a satellite at the horizon is
 * amplified by at most @f$1/\sin(el_{floor})@f$ (11.5 at the 5 degree default).
 *
 * @param elev_deg   Satellite elevation (deg).
 * @param floor_deg  Elevation floor (deg).
 * @return The dimensionless amplification factor (1 at zenith).
 */
inline double phaseColourElevFactor(double elev_deg, double floor_deg) noexcept {
    return 1.0 / std::sin(std::max(elev_deg, floor_deg) * (Constants::PI / 180.0));
}

/**
 * @brief Per-line-of-sight Gauss-Markov phase error generator for one station.
 *
 * Holds one process state per line-of-sight key (the satellite ID at the
 * simulator's call site) and a dedicated RNG stream.  See the file banner for the
 * model, the band-common contract and the byte-identity contract.
 */
class PhaseColourGenerator {
public:
    /**
     * @brief Construct the generator.
     *
     * @param sigma_zen_m     Zenith 1-sigma of the error (m).  0 = inactive: nothing
     *                        is drawn and every sample is exactly 0.
     * @param tau_s           Correlation time (s); must be > 0 (validated by the
     *                        config loader, not here).
     * @param elev_floor_deg  Elevation floor of the amplification factor (deg).
     * @param seed            Seed of the dedicated stream (already station-salted
     *                        by the caller).
     */
    PhaseColourGenerator(double sigma_zen_m, double tau_s, double elev_floor_deg, std::uint64_t seed)
        : sigma_zen_m_(sigma_zen_m), tau_s_(tau_s), elev_floor_deg_(elev_floor_deg),
          rng_(seed), gauss_(0.0, 1.0) {}

    /** @brief True when the block injects anything (@c sigma_zen_m > 0). */
    bool active() const noexcept { return sigma_zen_m_ > 0.0; }

    /**
     * @brief Advance one line of sight to time @p t_s and return its error.
     *
     * The first call for @p key is a stationary start (@f$u = g@f$).  A later call
     * advances the process across @f$\Delta t = t_s - t_{prev}@f$ (the time since
     * that key's previous call, i.e. its previous visible epoch, so a visibility gap
     * decorrelates it).  A call with @f$\Delta t \le 0@f$ is the same instant: it
     * returns the stored process value without drawing, so the realization is one
     * per (line of sight, epoch) however many bands query it.
     *
     * Inactive (@c sigma_zen_m == 0): returns a literal 0.0, draws nothing and keeps
     * no state.
     *
     * @param key       Line-of-sight identifier, e.g. "G05".
     * @param t_s       Continuous time of this epoch (s), any fixed origin.
     * @param elev_deg  Satellite elevation at this epoch (deg).
     * @return The error to add to every band's carrier phase (m).
     */
    double sample(const std::string& key, double t_s, double elev_deg) {
        if (!active()) return 0.0;
        auto it = state_.find(key);
        if (it == state_.end()) {
            it = state_.emplace(key, State{ draw(), t_s }).first;
            ++n_starts_;
        } else if (t_s - it->second.t_s > 0.0) {
            it->second.u = gaussMarkovUnitStep(it->second.u, t_s - it->second.t_s, tau_s_, draw());
            it->second.t_s = t_s;
        }
        last_u_ = it->second.u;
        return sigma_zen_m_ * phaseColourElevFactor(elev_deg, elev_floor_deg_) * last_u_;
    }

    /** @brief Unit process value returned with the most recent @ref sample (0 if none). */
    double lastUnit() const noexcept { return last_u_; }

    /** @brief Standard-normal draws taken from the stream so far (0 while inactive). */
    std::uint64_t draws() const noexcept { return n_draws_; }

    /** @brief Lines of sight started (first-sight stationary starts) so far. */
    std::uint64_t starts() const noexcept { return n_starts_; }

    /** @brief Zenith 1-sigma (m). */
    double sigmaZenM() const noexcept { return sigma_zen_m_; }
    /** @brief Correlation time (s). */
    double tauS() const noexcept { return tau_s_; }
    /** @brief Elevation floor of the amplification (deg). */
    double elevFloorDeg() const noexcept { return elev_floor_deg_; }

private:
    struct State {
        double u;    ///< Unit-variance process value.
        double t_s;  ///< Time of the sample that produced @c u (s).
    };

    double draw() {
        ++n_draws_;
        return gauss_(rng_);
    }

    double sigma_zen_m_;
    double tau_s_;
    double elev_floor_deg_;
    std::mt19937_64 rng_;
    std::normal_distribution<double> gauss_;
    std::map<std::string, State> state_;
    double last_u_{ 0.0 };
    std::uint64_t n_draws_{ 0 };
    std::uint64_t n_starts_{ 0 };
};

} // namespace sim
} // namespace GnssProcessor
