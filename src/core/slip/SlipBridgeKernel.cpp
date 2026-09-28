/**
 * @file SlipBridgeKernel.cpp
 * @brief Implementation of the non-causal GF+MW cycle-slip integer recovery.
 */
#include "slip/SlipBridgeKernel.hpp"

#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "math/TimeUtils.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

SlipBridgeKernel::Ordered SlipBridgeKernel::resolveOrder(
    System system, const std::string& sat, const std::string& sigA,
    const std::string& codeA, const std::string& sigB,
    const std::string& codeB) const {
    Ordered o;
    if (sigA.size() < 2 || sigB.size() < 2) return o;
    // Same band ⇒ geometry-free is degenerate (the band digit is char[1]).
    if (sigA[1] == sigB[1]) return o;

    const double fA = ppp_utils_.getFrequency(sigA, system, sat);
    const double fB = ppp_utils_.getFrequency(sigB, system, sat);
    if (!(std::isfinite(fA) && std::isfinite(fB) && fA > 0.0 && fB > 0.0)) return o;
    if (std::abs(fA - fB) < 1.0) return o;

    // Higher frequency first (sig1 = hi), keeping each code paired with its sig.
    if (fA >= fB) {
        o.sig_hi = sigA; o.code_hi = codeA; o.f_hi = fA;
        o.sig_lo = sigB; o.code_lo = codeB; o.f_lo = fB;
    } else {
        o.sig_hi = sigB; o.code_hi = codeB; o.f_hi = fB;
        o.sig_lo = sigA; o.code_lo = codeA; o.f_lo = fA;
    }
    o.lam_hi = ppp_utils_.getWavelength(system, o.sig_hi, sat);
    o.lam_lo = ppp_utils_.getWavelength(system, o.sig_lo, sat);
    if (!(o.lam_hi > 0.0) || !(o.lam_lo > 0.0)) return o;
    if (std::abs(o.lam_hi - o.lam_lo) < 1e-9) return o;
    o.ok = true;
    return o;
}

SlipBridgeKernel::BridgeWindows SlipBridgeKernel::buildWindows(
    const std::vector<RinexObservation>& all_obs, const std::string& sat,
    System system, const std::string& sigA, const std::string& sigB,
    const std::string& codeA, const std::string& codeB, double t_boundary_s,
    int half_window_epochs) const {
    BridgeWindows w;
    w.t_boundary_s = t_boundary_s;

    const Ordered o = resolveOrder(system, sat, sigA, codeA, sigB, codeB);
    if (!o.ok || half_window_epochs <= 0) return w;

    const double c = Constants::SPEED_OF_LIGHT;
    const double f_diff = o.f_hi - o.f_lo;
    const double f_sum = o.f_hi + o.f_lo;
    if (std::abs(f_diff) < 1.0 || std::abs(f_sum) < 1.0) return w;

    struct Sample { double t; double gf; double mw; };
    std::vector<Sample> samples;
    samples.reserve(all_obs.size());

    for (const auto& obs : all_obs) {
        if (obs.satellite != sat) continue;
        const auto itLh = obs.measurements.find(o.sig_hi);
        const auto itLl = obs.measurements.find(o.sig_lo);
        const auto itPh = obs.measurements.find(o.code_hi);
        const auto itPl = obs.measurements.find(o.code_lo);
        if (itLh == obs.measurements.end() || itLl == obs.measurements.end() ||
            itPh == obs.measurements.end() || itPl == obs.measurements.end())
            continue;

        const double L_hi = itLh->second;  // cycles
        const double L_lo = itLl->second;  // cycles
        const double P_hi = itPh->second;  // metres
        const double P_lo = itPl->second;  // metres
        if (!std::isfinite(L_hi) || !std::isfinite(L_lo) ||
            !std::isfinite(P_hi) || !std::isfinite(P_lo))
            continue;

        // GF (metres), as in TurboEditResolver::computeGfMeters.
        const double gf = L_hi * o.lam_hi - L_lo * o.lam_lo;
        // MW (metres), as in MwSlipDetector::computeMwMeters.
        const double L_hi_m = L_hi * c / o.f_hi;
        const double L_lo_m = L_lo * c / o.f_lo;
        const double wl_phase_m = (o.f_hi * L_hi_m - o.f_lo * L_lo_m) / f_diff;
        const double nl_code_m = (o.f_hi * P_hi + o.f_lo * P_lo) / f_sum;
        const double mw = wl_phase_m - nl_code_m;
        if (!std::isfinite(gf) || !std::isfinite(mw)) continue;

        const double t = TimeUtils::continuousGpsSeconds(obs.time);
        if (!std::isfinite(t)) continue;
        samples.push_back({t, gf, mw});
    }

    std::sort(samples.begin(), samples.end(),
              [](const Sample& a, const Sample& b) { return a.t < b.t; });

    // First sample at/after the boundary.
    std::size_t split = samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].t >= t_boundary_s) { split = i; break; }
    }
    const std::size_t pre_begin =
        (split > static_cast<std::size_t>(half_window_epochs))
            ? split - static_cast<std::size_t>(half_window_epochs)
            : 0;
    const std::size_t post_end =
        std::min(samples.size(),
                 split + static_cast<std::size_t>(half_window_epochs));

    for (std::size_t i = pre_begin; i < split; ++i) {
        w.gf_pre.t_s.push_back(samples[i].t);
        w.gf_pre.y_m.push_back(samples[i].gf);
        w.mw_pre.t_s.push_back(samples[i].t);
        w.mw_pre.y_m.push_back(samples[i].mw);
    }
    for (std::size_t i = split; i < post_end; ++i) {
        w.gf_post.t_s.push_back(samples[i].t);
        w.gf_post.y_m.push_back(samples[i].gf);
        w.mw_post.t_s.push_back(samples[i].t);
        w.mw_post.y_m.push_back(samples[i].mw);
    }

    w.ok = !w.gf_pre.t_s.empty() && !w.gf_post.t_s.empty() &&
           !w.mw_pre.t_s.empty() && !w.mw_post.t_s.empty();
    return w;
}

SlipBridgeKernel::RecoveredSlip SlipBridgeKernel::recover(
    const BridgeWindows& w, const std::string& sat, System system,
    const std::string& sigA, const std::string& sigB,
    const JumpGapFitter& fitter, double frac_accept_cyc,
    int max_abs_cycles) const {
    RecoveredSlip r;
    if (!w.ok) return r;

    // Codes are irrelevant to the integer algebra; pass empty placeholders.
    const Ordered o = resolveOrder(system, sat, sigA, "", sigB, "");
    if (!o.ok) return r;

    const JumpGapFitter::FitResult gf =
        fitter.fitStep(w.gf_pre, w.gf_post, w.t_boundary_s);
    const JumpGapFitter::FitResult mw =
        fitter.fitStep(w.mw_pre, w.mw_post, w.t_boundary_s);
    if (!gf.ok || !mw.ok) return r;

    const double c = Constants::SPEED_OF_LIGHT;
    const double lambda_wl = c / std::abs(o.f_hi - o.f_lo);
    if (!(lambda_wl > 0.0)) return r;

    const double denom = o.lam_hi - o.lam_lo;  // distinct bands ⇒ nonzero
    if (std::abs(denom) < 1e-9) return r;

    const double wl_real = mw.step_m / lambda_wl;
    const long dN_wl = std::lround(wl_real);
    const double dN2_real = (gf.step_m - o.lam_hi * static_cast<double>(dN_wl)) / denom;
    const long dN2 = std::lround(dN2_real);
    const long dN1 = dN2 + dN_wl;

    const double frac_wl = std::abs(wl_real - static_cast<double>(dN_wl));
    const double frac_gf = std::abs(dN2_real - static_cast<double>(dN2));
    const double frac = std::max(frac_wl, frac_gf);

    if (frac > frac_accept_cyc) return r;
    if (std::abs(dN1) > max_abs_cycles || std::abs(dN2) > max_abs_cycles) return r;
    if (dN1 == 0 && dN2 == 0) return r;  // no slip recovered

    r.ok = true;
    r.dN1 = static_cast<int>(dN1);
    r.dN2 = static_cast<int>(dN2);
    r.dN_wl = static_cast<int>(dN_wl);
    r.frac_resid_cyc = frac;
    r.gf_step_m = gf.step_m;
    r.mw_step_m = mw.step_m;
    r.lambda_wl_m = lambda_wl;
    r.sig1 = o.sig_hi;
    r.sig2 = o.sig_lo;
    return r;
}

}  // namespace GnssProcessor
