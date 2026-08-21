/**
 * @file NetworkRawObservables.cpp
 * @brief Shared RAW cycle-slip observables + the empirical qualification pass
 *        (Cards L and G of the 2026-07-21 slip-QC hardening).
 *
 * @ref networkComputeRawObservables is a VERBATIM lift of the arithmetic that
 * used to live inside the `network.trace_slip_raw` writer in
 * ApplicationNetworkAssembly.cpp — same signal selection, same sort, same
 * formulas, same skip conditions — so the trace rows are byte-unchanged when
 * the trace is the only consumer.  Everything below it is new.
 */

#include "slip/NetworkRawObservables.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

namespace {

/// Does this observation carry at least one phase signal at all?  (Denominator
/// of the Card-L LLI-set fraction: a satellite with no phase cannot assert a
/// phase loss-of-lock, so counting it would dilute a chatty station's score.)
bool hasAnyPhase(const PppObservation& obs) {
    for (const auto& kv : obs.raw_observed)
        if (!kv.first.empty() && kv.first[0] == 'L') return true;
    return false;
}

/// Does this observation assert RINEX LLI bit 0 on any phase signal?
bool assertsLli(const PppObservation& obs) {
    for (const auto& [code, lli] : obs.lli_flags)
        if (!code.empty() && code[0] == 'L' && (lli & 1)) return true;
    return false;
}

}  // namespace

// =====================================================================
// Raw observable computation (verbatim from the former trace writer)
// =====================================================================

void networkComputeRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  PppUtils& utils,
                                  RawSlipEpoch& out) {
    for (int i = 0; i < n_sta_total; ++i) {
        if (i >= static_cast<int>(out.rows.size())) break;
        if (!present[i]) continue;
        auto& dst = out.rows[static_cast<std::size_t>(i)];
        for (const auto& obs : modeled[static_cast<std::size_t>(i)]) {
            std::vector<std::string> ph;  // phase signals with a matching code
            for (const auto& kv : obs.raw_observed) {
                const std::string& k = kv.first;
                if (k.size() < 2 || k[0] != 'L') continue;
                if (obs.raw_observed.count("C" + k.substr(1))) ph.push_back(k);
            }
            if (ph.empty()) continue;
            std::sort(ph.begin(), ph.end(),
                      [](const std::string& a, const std::string& b) { return a[1] < b[1]; });
            const std::string s1 = ph.front();
            std::string s2;
            for (const auto& k : ph) if (k[1] != s1[1]) { s2 = k; break; }  // distinct band

            const double lam1 = utils.getWavelength(obs.system, s1, obs.satellite);
            if (!(std::isfinite(lam1) && lam1 > 0.0)) continue;
            const double L1_cyc = obs.raw_observed.at(s1);
            const double P1_m   = obs.raw_observed.at("C" + s1.substr(1));
            const double L1_m   = L1_cyc * lam1;
            const double l1c1   = L1_m - P1_m;
            const int    lli1   = obs.lli_flags.count(s1) ? obs.lli_flags.at(s1) : 0;

            bool dual = !s2.empty();
            double mw = 0.0, gf = 0.0, lam_wl = 0.0;
            double gf_code = 0.0, gf_denom = 0.0;
            int    lli2 = 0;
            if (dual) {
                const double lam2 = utils.getWavelength(obs.system, s2, obs.satellite);
                if (std::isfinite(lam2) && lam2 > 0.0) {
                    const double inv1 = 1.0 / lam1, inv2 = 1.0 / lam2;
                    const double L2_cyc = obs.raw_observed.at(s2);
                    const double P2_m   = obs.raw_observed.at("C" + s2.substr(1));
                    gf = L1_m - L2_cyc * lam2;
                    mw = (L1_cyc - L2_cyc) / (inv1 - inv2)
                       - (P1_m * inv1 + P2_m * inv2) / (inv1 + inv2);
                    lli2 = obs.lli_flags.count(s2) ? obs.lli_flags.at(s2) : 0;
                    // CS-GF authority inputs (QUEUE 8b successor): the code GF
                    // and the pair's dispersive scale γ−1.  Struct fields only —
                    // the slip_raw trace writer prints its explicit column list,
                    // so an OFF run stays byte-identical.
                    gf_code = P1_m - P2_m;
                    const double g = (lam2 / lam1) * (lam2 / lam1);
                    gf_denom = g - 1.0;
                    // Wide-lane wavelength — the PHYSICAL unit the MW floor is
                    // expressed in (0.5 WL cycles).  Exact for FDMA too, since
                    // both wavelengths came from the satellite-aware lookup.
                    const double dinv = inv1 - inv2;
                    if (std::abs(dinv) > 1e-12) lam_wl = std::abs(1.0 / dinv);
                } else {
                    dual = false;
                }
            }

            RawSlipObs r;
            r.satellite   = obs.satellite;
            r.sig1        = s1;
            r.sig2        = dual ? s2 : std::string{};
            r.elev_deg    = obs.elevation;
            r.dual        = dual;
            r.mw_m        = mw;
            r.gf_m        = gf;
            r.gf_code_m   = gf_code;
            r.gf_denom    = gf_denom;
            r.lam1_m      = lam1;
            r.l1_c1_m     = l1c1;
            r.lambda_wl_m = lam_wl;
            r.lli1        = lli1;
            r.lli2        = lli2;
            dst.push_back(std::move(r));
        }
    }
    out.computed = true;
}

// =====================================================================
// Pure helpers (unit-testable without a station)
// =====================================================================

bool networkIonoCsUpdate(IonoCsState& cs, double i_code_m, double di_carrier_m,
                         bool adjacent, bool stepped, double sigma_i_code_m) {
    // Reset ladder (invariant #1 for this authority): lost adjacency (gap /
    // first sighting / pair change tripping the gate), an empirical MW/GF step
    // (slip class), or a never-initialized state.  The reset re-seeds from the
    // code sample alone at its own honest variance — briefly code-noisy, never
    // level-contaminated.
    if (!adjacent || stepped || cs.n <= 0 || !std::isfinite(cs.val)) {
        if (!std::isfinite(i_code_m) || !(sigma_i_code_m > 0.0)) {
            cs.n = 0;   // nothing usable this epoch — stay uninitialized
            return true;
        }
        cs.val = i_code_m;
        cs.var = sigma_i_code_m * sigma_i_code_m;
        cs.n = 1;
        return true;
    }
    // Carrier propagation: the GF-phase delta tracks the true iono change
    // exactly (ambiguities constant on an un-slipped arc).
    if (std::isfinite(di_carrier_m)) {
        cs.val += di_carrier_m;
        cs.var += kIonoCsPhaseStepVar;
    }
    // Kalman blend toward this epoch's code sample.
    if (std::isfinite(i_code_m) && sigma_i_code_m > 0.0) {
        const double r = sigma_i_code_m * sigma_i_code_m;
        const double w = cs.var / (cs.var + r);
        cs.val += w * (i_code_m - cs.val);
        cs.var *= (1.0 - w);
        if (cs.n < 1000000) ++cs.n;
    }
    return false;
}

double networkRobustStepScale(std::vector<double>& abs_steps) {
    if (abs_steps.empty()) return 0.0;
    const std::size_t mid = abs_steps.size() / 2;
    std::nth_element(abs_steps.begin(), abs_steps.begin() + static_cast<long>(mid),
                     abs_steps.end());
    return 1.4826 * abs_steps[mid];
}

bool networkLliCredibilityStep(bool credible_in, double frac_ewma, long count,
                               const SlipQcParams& p) {
    // Arming gate: below min_epochs of history the verdict never moves — a
    // single noisy epoch must not demote a station, and (symmetrically) a
    // station cannot be rehabilitated on one quiet epoch either.
    if (count < static_cast<long>(p.chatter_min_epochs)) return credible_in;
    if (credible_in) {
        // Demote only above the HIGH threshold: an implausible sustained
        // fraction of the station's tracked satellites asserting loss-of-lock.
        return !(frac_ewma > p.chatter_frac_hi);
    }
    // Promote back below the LOW threshold (hysteresis band between the two).
    return (frac_ewma < p.chatter_frac_lo);
}

// =====================================================================
// Qualification pass (serial, station-ordered → deterministic)
// =====================================================================

void networkQualifyRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  long long epoch_idx,
                                  const SlipQcParams& p,
                                  std::vector<RawSlipHistory>& hist,
                                  RawSlipEpoch& ep,
                                  SlipQcCounters& cnt) {
    if (!ep.computed) return;
    if (hist.size() < ep.rows.size()) hist.resize(ep.rows.size());

    for (int i = 0; i < n_sta_total; ++i) {
        const std::size_t si = static_cast<std::size_t>(i);
        if (si >= ep.rows.size()) break;
        if (!present[i]) continue;
        RawSlipHistory& h = hist[si];
        const auto& rows  = ep.rows[si];
        auto& vmap        = ep.verdict[si];

        // ---- (1) Adjacent-epoch steps -----------------------------------
        // Only an IMMEDIATELY-adjacent predecessor forms a step.  A row whose
        // predecessor is older is a RESUMPTION (handled at (5)); mixing the two
        // would let a multi-hour gap poison the noise scale.
        std::vector<double> mw_abs, gf_abs;
        mw_abs.reserve(rows.size());
        gf_abs.reserve(rows.size());
        for (const auto& r : rows) {
            RawSlipVerdict v;
            const auto pit = h.prev.find(r.satellite);
            if (pit != h.prev.end() && pit->second.epoch_idx == epoch_idx - 1
                && pit->second.obs.dual && r.dual) {
                v.measured = true;
                v.d_mw = r.mw_m - pit->second.obs.mw_m;
                v.d_gf = r.gf_m - pit->second.obs.gf_m;
                mw_abs.push_back(std::abs(v.d_mw));
                gf_abs.push_back(std::abs(v.d_gf));
            }
            vmap[r.satellite] = v;
        }

        // ---- (2) Pre-update effective scales ----------------------------
        // Cold start (no folded history): this epoch's OWN robust median is the
        // prior — self-calibrating, and never a stochastic-model σ (rule 4).
        std::vector<double> mw_tmp = mw_abs, gf_tmp = gf_abs;
        const double s_mw = (h.mw_step_count > 0) ? h.mw_step_scale
                                                  : networkRobustStepScale(mw_tmp);
        const double s_gf = (h.gf_step_count > 0) ? h.gf_step_scale
                                                  : networkRobustStepScale(gf_tmp);

        // ---- (3) Threshold with the PRE-update scale --------------------
        std::vector<double> mw_clean, gf_clean;
        for (const auto& r : rows) {
            auto& v = vmap[r.satellite];
            v.gate_mw = std::max(p.k_mw * s_mw, p.mw_floor_cyc * r.lambda_wl_m);
            v.gate_gf = std::max(p.k_gf * s_gf, p.gf_floor_m);
            if (!v.measured) continue;
            v.mw_step = (v.gate_mw > 0.0) && (std::abs(v.d_mw) > v.gate_mw);
            v.gf_step = (v.gate_gf > 0.0) && (std::abs(v.d_gf) > v.gate_gf);
            v.stepped = v.mw_step || v.gf_step;
            if (v.stepped) ep.stepped.insert({i, r.satellite});
            // Clean survivors only — the scale must never absorb the event it
            // exists to catch (the IF-TD pool arm's discipline, restated).
            if (!v.mw_step) mw_clean.push_back(std::abs(v.d_mw));
            if (!v.gf_step) gf_clean.push_back(std::abs(v.d_gf));
        }

        // ---- (4a) Fold the clean survivors into the per-station EWMAs ----
        if (!mw_clean.empty()) {
            const double s_epoch = networkRobustStepScale(mw_clean);
            h.mw_step_scale = (h.mw_step_count == 0)
                ? s_epoch
                : (1.0 - p.step_alpha) * h.mw_step_scale + p.step_alpha * s_epoch;
            h.mw_step_count += static_cast<long>(mw_clean.size());
        }
        if (!gf_clean.empty()) {
            const double s_epoch = networkRobustStepScale(gf_clean);
            h.gf_step_scale = (h.gf_step_count == 0)
                ? s_epoch
                : (1.0 - p.step_alpha) * h.gf_step_scale + p.step_alpha * s_epoch;
            h.gf_step_count += static_cast<long>(gf_clean.size());
        }

        // ---- (4b) Card L credibility -------------------------------------
        // Fraction over the satellites that CAN assert a phase LLI at this
        // station this epoch.  Folded first, then the two-state hysteresis is
        // evaluated on the post-fold EWMA, so a station that is chattering on
        // its very first epoch is judged on that epoch's own evidence rather
        // than after an arbitrary warmup (its arcs are fresh then anyway — the
        // carry has nothing to protect, so a suppression there costs nothing).
        int n_phase = 0, n_lli = 0;
        for (const auto& obs : modeled[si]) {
            if (!hasAnyPhase(obs)) continue;
            ++n_phase;
            if (assertsLli(obs)) ++n_lli;
        }
        if (n_phase > 0) {
            const double frac = static_cast<double>(n_lli) / static_cast<double>(n_phase);
            ep.lli_frac[si] = frac;
            h.lli_frac_ewma = (h.lli_frac_count == 0)
                ? frac
                : (1.0 - p.chatter_alpha) * h.lli_frac_ewma + p.chatter_alpha * frac;
            ++h.lli_frac_count;
            const bool was = h.lli_credible;
            h.lli_credible = networkLliCredibilityStep(was, h.lli_frac_ewma,
                                                       h.lli_frac_count, p);
            if (was && !h.lli_credible) ++h.lli_demotions;
            if (!was && h.lli_credible) ++h.lli_promotions;
        }
        ep.lli_credible[si] = h.lli_credible ? 1 : 0;
        if (h.lli_credible) ++cnt.lli_credible_epochs;
        else                ++cnt.lli_demoted_epochs;

        // ---- (5) Card G: arcs resuming after a tracking break -------------
        if (p.gap_instrument) {
            for (const auto& r : rows) {
                const auto pit = h.prev.find(r.satellite);
                if (pit == h.prev.end()) continue;                       // first sighting
                if (pit->second.epoch_idx >= epoch_idx - 1) continue;    // adjacent (or same) — not a gap
                GapResumption g;
                g.station    = i;
                g.satellite  = r.satellite;
                g.sig1       = r.sig1;
                g.sig2       = r.sig2;
                g.gap_epochs = epoch_idx - pit->second.epoch_idx - 1;
                g.measurable = pit->second.obs.dual && r.dual;
                if (g.measurable) {
                    g.d_mw = r.mw_m - pit->second.obs.mw_m;
                    g.d_gf = r.gf_m - pit->second.obs.gf_m;
                    const double gate_mw = std::max(p.k_mw * s_mw,
                                                    p.mw_floor_cyc * r.lambda_wl_m);
                    const double gate_gf = std::max(p.k_gf * s_gf, p.gf_floor_m);
                    g.shifted = (gate_mw > 0.0 && std::abs(g.d_mw) > gate_mw)
                             || (gate_gf > 0.0 && std::abs(g.d_gf) > gate_gf);
                }
                ++cnt.gap_resumptions;
                if (!g.measurable)   ++cnt.gap_unmeasurable;
                else if (g.shifted)  ++cnt.gap_shifted;
                ep.resumptions.push_back(std::move(g));
            }
        }

        // ---- (6) Advance the history -------------------------------------
        for (const auto& r : rows) {
            auto& slot = h.prev[r.satellite];
            slot.obs       = r;
            slot.epoch_idx = epoch_idx;
        }
    }
}

} // namespace GnssProcessor
