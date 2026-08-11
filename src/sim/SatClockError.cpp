/**
 * @file SatClockError.cpp
 * @brief Implementation of the synthetic satellite-clock error channel.
 *
 * See SatClockError.hpp for the determinism contract and the injection-site
 * hazards; this file only realizes it.
 */

#include "sim/SatClockError.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>

#include "common/Constants.hpp"

namespace GnssProcessor {
namespace sim {

namespace {

/// splitmix64 — the same mixer the simulator's other self-contained hashes use.
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/// Per-satellite stream seed.  Derived from (base seed, satellite id) ONLY, so a
/// satellite's realization is independent of which other satellites are present
/// and of their iteration order — the property that lets N station processes
/// agree bit-for-bit on the satellite side.
std::uint64_t satSeed(std::uint64_t base, const std::string& sat) {
    std::uint64_t h = mix(base ^ 0x5A7C10CBull);
    for (unsigned char ch : sat) h = mix(h ^ static_cast<std::uint64_t>(ch));
    return h;
}

/// Elapsed seconds from @p a to @p b across a possible week rollover.
double elapsed(const GnssTime& a, const GnssTime& b) {
    return (b.week - a.week) * 604800.0 + (b.tow - a.tow);
}

} // namespace

bool SatClockError::build(const SimConfig::SatClock& cfg,
                          const std::vector<std::string>& sats,
                          const GnssTime& t0, double interval_s, int n_epochs,
                          std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };

    table_.clear();
    active_ = false;
    t0_ = t0;
    interval_s_ = interval_s;
    n_epochs_ = n_epochs;

    if (interval_s <= 0.0) return fail("SatClockError: interval_s must be > 0");
    if (n_epochs <= 0) return fail("SatClockError: n_epochs must be > 0");

    const bool stochastic = (cfg.bias0_sigma_m > 0.0) || (cfg.rate_sigma_mps > 0.0) ||
                            (cfg.rw_q_m2ps > 0.0);
    if (!stochastic && cfg.steps.empty()) return true;   // inactive; metres() == 0.0

    // A scripted step naming a satellite that is not generated would inject nothing
    // while the event matrix still lists it — and an event that injects nothing is
    // graded as an UNDETECTED event, i.e. a false guard failure.  Refuse loudly.
    const std::set<std::string> known(sats.begin(), sats.end());
    for (const auto& ev : cfg.steps)
        if (!known.count(ev.sat))
            return fail("SatClockError: sat_clock.steps names satellite '" + ev.sat +
                        "' which is not in this run's signal plan — the step would inject "
                        "nothing and grade as an undetected event");

    for (const auto& sat : sats) {
        std::vector<double> series(static_cast<std::size_t>(n_epochs), 0.0);

        if (stochastic) {
            std::mt19937_64 rng(satSeed(cfg.seed, sat));
            std::normal_distribution<double> g(0.0, 1.0);

            // Drawn ONCE per satellite, in a fixed order, so adding a satellite to the
            // plan cannot perturb another satellite's realization.
            const double bias0 = cfg.bias0_sigma_m > 0.0 ? cfg.bias0_sigma_m * g(rng) : 0.0;
            const double rate = cfg.rate_sigma_mps > 0.0 ? cfg.rate_sigma_mps * g(rng) : 0.0;
            // Random walk: sigma per step = sqrt(q * dt), the same convention the
            // producer's filter.process_noise.sat_clock_q uses, so the injected truth and
            // the estimator's stochastic model are directly comparable.  The first epoch
            // is undisturbed (w_0 = 0), matching the receiver-clock walk.
            const double rw_step = cfg.rw_q_m2ps > 0.0
                                       ? std::sqrt(cfg.rw_q_m2ps * interval_s) : 0.0;
            double walk = 0.0;
            for (int k = 0; k < n_epochs; ++k) {
                if (k > 0 && rw_step > 0.0) walk += rw_step * g(rng);
                series[static_cast<std::size_t>(k)] = bias0 + rate * (k * interval_s) + walk;
            }
        }

        // Scripted steps, layered on top.  A step is HELD for the remainder of the arc
        // (a step, not an impulse); ramp_mps accumulates from the step epoch onward.
        for (const auto& ev : cfg.steps) {
            if (ev.sat != sat) continue;
            const int k0 = ev.epoch - 1;                       // config is 1-based
            for (int k = std::max(0, k0); k < n_epochs; ++k)
                series[static_cast<std::size_t>(k)] +=
                    ev.step_m + ev.ramp_mps * ((k - k0) * interval_s);
        }

        table_.emplace(sat, std::move(series));
    }

    active_ = true;
    return true;
}

double SatClockError::metres(const std::string& sat, const GnssTime& t) const {
    if (!active_) return 0.0;
    const auto it = table_.find(sat);
    if (it == table_.end()) return 0.0;
    const auto& s = it->second;
    if (s.empty()) return 0.0;

    const double x = elapsed(t0_, t) / interval_s_;
    if (x <= 0.0) return s.front();                              // emission time of epoch 1
    const double last = static_cast<double>(s.size() - 1);
    if (x >= last) return s.back();

    const std::size_t k = static_cast<std::size_t>(x);
    const double f = x - static_cast<double>(k);
    return s[k] * (1.0 - f) + s[k + 1] * f;
}

double SatClockError::peakAbsMetres() const {
    double peak = 0.0;
    for (const auto& [sat, s] : table_)
        for (double v : s) peak = std::max(peak, std::abs(v));
    return peak;
}

bool SatClockError::writeTruth(const std::filesystem::path& path) const {
    if (!active_) return true;   // absent file == "channel not simulated"
    std::ofstream out(path);
    if (!out) return false;
    out << "week,tow_s,satellite,clk_err_m,clk_err_s\n" << std::fixed;
    const double c = Constants::SPEED_OF_LIGHT;
    for (int k = 0; k < n_epochs_; ++k) {
        GnssTime t = t0_;
        t.tow += k * interval_s_;
        while (t.tow >= 604800.0) { t.tow -= 604800.0; ++t.week; }
        for (const auto& [sat, s] : table_) {
            if (static_cast<std::size_t>(k) >= s.size()) continue;
            out << t.week << "," << std::setprecision(3) << t.tow << "," << sat << ","
                << std::setprecision(9) << s[static_cast<std::size_t>(k)] << ","
                << std::setprecision(15) << (s[static_cast<std::size_t>(k)] / c) << "\n";
        }
    }
    return true;
}

} // namespace sim
} // namespace GnssProcessor
