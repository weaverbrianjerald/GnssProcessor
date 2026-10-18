#include "corrections/WindUpGapBridge.hpp"

#include "common/Constants.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {
namespace WindUpBridge {

namespace {

constexpr double kTwoPi = 2.0 * Constants::PI;

/// Wrap an angle difference into [-pi, pi].
double wrapPi(double x) {
    return x - kTwoPi * std::round(x / kTwoPi);
}

}  // namespace

const char* outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Bridged:      return "bridged";
        case Outcome::NoState:      return "no_state";
        case Outcome::Umbra:        return "umbra";
        case Outcome::NoonTurn:     return "noon_turn";
        case Outcome::StepTooLarge: return "step_too_large";
        case Outcome::Closure:      return "closure";
    }
    return "unknown";
}

void Counters::record(const Result& r) {
    ++attempts;
    samples += r.samples;
    switch (r.outcome) {
        case Outcome::Bridged:
            ++bridged;
            if (r.cycles != 0) ++bridged_stepped;
            break;
        case Outcome::NoState:      ++refused_no_state;   break;
        case Outcome::Umbra:        ++refused_umbra;      break;
        case Outcome::NoonTurn:     ++refused_noon_turn;  break;
        case Outcome::StepTooLarge: ++refused_step;       break;
        case Outcome::Closure:      ++refused_closure;    break;
    }
}

void Counters::add(const Counters& o) {
    attempts          += o.attempts;
    bridged           += o.bridged;
    bridged_stepped   += o.bridged_stepped;
    refused_no_state  += o.refused_no_state;
    refused_umbra     += o.refused_umbra;
    refused_noon_turn += o.refused_noon_turn;
    refused_step      += o.refused_step;
    refused_closure   += o.refused_closure;
    beyond_horizon    += o.beyond_horizon;
    samples           += o.samples;
}

Result walk(double t_prev_s, double t_now_s,
            double angle_prev_rad, double angle_now_rad,
            const Options& opt, const Sampler& sampler) {
    Result res;
    const double gap = t_now_s - t_prev_s;
    if (!(gap > 0.0) || !sampler) return res;   // NoState

    // Equal steps no longer than step_s.  The count is bounded so that a
    // mis-set step cannot make one gap cost an unbounded number of evaluations.
    const double step = std::max(opt.step_s, 1e-3);
    // The small offset keeps a gap of exactly n steps, which continuous seconds near
    // 1.4e9 carry to within a few 1e-7 s, from being cut into n + 1.
    const int n = static_cast<int>(std::clamp(std::ceil(gap / step - 1e-6), 1.0, 20000.0));
    const double dt = gap / static_cast<double>(n);

    // A sample that cannot be used ends the attempt with the reason it failed.
    auto usable = [&](const Sample& s) -> bool {
        ++res.samples;
        if (!s.ok)        { res.outcome = Outcome::NoState;  return false; }
        if (s.umbra)      { res.outcome = Outcome::Umbra;    return false; }
        if (s.noon_turn)  { res.outcome = Outcome::NoonTurn; return false; }
        return true;
    };

    Sample prev = sampler(t_prev_s);
    if (!usable(prev)) return res;

    double walked = 0.0;   // Sum of the unwrapped steps (rad).
    for (int i = 1; i <= n; ++i) {
        const double t_end = (i == n) ? t_now_s : t_prev_s + dt * static_cast<double>(i);
        const double t_mid = t_prev_s + dt * (static_cast<double>(i) - 0.5);
        const Sample mid = sampler(t_mid);
        if (!usable(mid)) return res;
        const Sample end = sampler(t_end);
        if (!usable(end)) return res;

        // Resolving the step at its middle makes a change of half a cycle or more
        // visible, where the wrapped step alone would read it as its alias.  Each
        // half step is held to a quarter cycle: a half step of 0.55 cycle wraps to
        // -0.45 and would cancel against a next half step of 0.05 in the sum, but
        // not against this limit.
        const double half1 = wrapPi(mid.angle_rad - prev.angle_rad);
        const double half2 = wrapPi(end.angle_rad - mid.angle_rad);
        const double half_limit = 0.5 * kMaxStepCycles * kTwoPi;
        if (std::abs(half1) >= half_limit || std::abs(half2) >= half_limit) {
            res.outcome = Outcome::StepTooLarge;
            return res;
        }
        walked += half1 + half2;
        prev = end;
    }

    // The integer part comes from the walk, the fractional part from the data.
    const double offset = angle_prev_rad + walked - angle_now_rad;
    const double whole = std::round(offset / kTwoPi);
    if (std::abs(offset - whole * kTwoPi) > kClosureCycles * kTwoPi) {
        res.outcome = Outcome::Closure;
        return res;
    }
    res.outcome = Outcome::Bridged;
    res.cycles = static_cast<long long>(whole);
    res.angle_rad = angle_now_rad + whole * kTwoPi;
    return res;
}

}  // namespace WindUpBridge
}  // namespace GnssProcessor
