/**
 * @file AmbLevelCheck.cpp
 * @brief Implementation of the windowed code-level re-validation (card ALC).
 *
 * See AmbLevelCheck.hpp for the statistic, the test and the restart contract.
 */

#include "realtime/AmbLevelCheck.hpp"

#include <cmath>

namespace GnssProcessor {

const char* AmbLevelCheck::testName(Test t) {
    switch (t) {
        case Test::Wl: return "wl";
        case Test::If: return "if";
        default:       return "";
    }
}

bool AmbLevelCheck::restart(const std::string& sat) {
    const auto it = win_.find(sat);
    if (it == win_.end() || (it->second.wl.empty() && it->second.ifd.empty())) return false;
    it->second.wl.clear();
    it->second.ifd.clear();
    ++ctr_.restarts;
    return true;
}

std::size_t AmbLevelCheck::depth(const std::string& sat) const {
    const auto it = win_.find(sat);
    return it == win_.end() ? 0u : it->second.wl.size();
}

void AmbLevelCheck::moments(const std::deque<double>& v, double floor,
                            double& mean, double& sigma) {
    mean = 0.0;
    sigma = floor;
    const std::size_t n = v.size();
    if (n == 0) return;
    double s = 0.0;
    for (double x : v) s += x;
    mean = s / static_cast<double>(n);
    if (n < 2) return;
    double ss = 0.0;
    for (double x : v) { const double d = x - mean; ss += d * d; }
    // Sample sigma (n-1), then the standard error of the mean, then the floor.
    const double s_sample = std::sqrt(ss / static_cast<double>(n - 1));
    const double s_mean   = s_sample / std::sqrt(static_cast<double>(n));
    sigma = (s_mean > floor) ? s_mean : floor;
}

AmbLevelCheck::Verdict AmbLevelCheck::push(const std::string& sat,
                                           double wl_cyc, double if_m) {
    Verdict v{};
    if (cfg_.window_ep < 2) return v;               // a window of < 2 has no sigma
    if (!std::isfinite(wl_cyc) || !std::isfinite(if_m)) return v;

    const auto ins = win_.emplace(sat, Window{});
    if (ins.second) ++ctr_.sats;
    Window& w = ins.first->second;

    w.wl.push_back(wl_cyc);
    w.ifd.push_back(if_m);
    ++ctr_.samples;
    const std::size_t cap = static_cast<std::size_t>(cfg_.window_ep);
    while (w.wl.size()  > cap) w.wl.pop_front();
    while (w.ifd.size() > cap) w.ifd.pop_front();

    if (w.wl.size() < cap) return v;                // still filling

    ++ctr_.checks;
    v.tested = true;
    v.n = static_cast<int>(w.wl.size());
    moments(w.wl,  kSigmaFloorWlCyc, v.wl_mean, v.wl_sigma);
    moments(w.ifd, kSigmaFloorIfM,   v.if_mean, v.if_sigma);

    // WL first: the coarse, integer-bearing test.  An IF trip on the same
    // satellite in the same epoch would name the same defect twice.
    if (std::fabs(v.wl_mean) > cfg_.k * v.wl_sigma) {
        v.test = Test::Wl; v.diff = std::fabs(v.wl_mean); v.sigma = v.wl_sigma;
        ++ctr_.trips_wl;
    } else if (std::fabs(v.if_mean) > cfg_.k * v.if_sigma) {
        v.test = Test::If; v.diff = std::fabs(v.if_mean); v.sigma = v.if_sigma;
        ++ctr_.trips_if;
    }

    // A trip clears the window in both modes (class banner).  In `act` the clear
    // is redundant with the restart that follows the refloat; in `observe` it
    // makes the counters record distinct trip events.
    if (v.test != Test::None) { w.wl.clear(); w.ifd.clear(); }
    return v;
}

} // namespace GnssProcessor
