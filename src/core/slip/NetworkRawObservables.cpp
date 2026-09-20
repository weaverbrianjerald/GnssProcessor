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
                                  RawSlipEpoch& out,
                                  bool per_signal) {
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
            // ---- Card GRS: the per-SIGNAL view of the same observation ------
            // Everything above summarises the satellite through ONE pair.  The
            // per-signal re-entry tier needs the signals the pair leaves out,
            // because those are precisely what keeps the row alive across a
            // primary-pair loss of lock.  `ph` is already the lowest-band-first
            // list of phases WITH a matching code, so the two views can never
            // disagree about which signals exist.  A signal whose wavelength
            // does not resolve is dropped rather than carried in cycles: the
            // level test differences metres.
            if (per_signal) {
                r.sigs.reserve(ph.size());
                for (const auto& k : ph) {
                    const double lam = utils.getWavelength(obs.system, k, obs.satellite);
                    if (!(std::isfinite(lam) && lam > 0.0)) continue;
                    RawSigObs s;
                    s.sig     = k;
                    s.phase_m = obs.raw_observed.at(k) * lam;
                    s.code_m  = obs.raw_observed.at("C" + k.substr(1));
                    s.lli     = obs.lli_flags.count(k) ? obs.lli_flags.at(k) : 0;
                    r.sigs.push_back(std::move(s));
                }
            }
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
                // The step is real, but if the PAIR changed the two MW values sit
                // on different wide-lane wavelengths — d_mw is still a level
                // change, yet converting it to cycles of THIS pair's lane is
                // meaningless.  Flagged, not suppressed: the slip cards want the
                // step; the lane observatory refuses it.
                v.pair_changed = (pit->second.obs.sig1 != r.sig1)
                              || (pit->second.obs.sig2 != r.sig2);
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
        // Armed by the INSTRUMENT (gap_instrument) or by the RESPONSE
        // (gap_reentry, docs/SPEC_slip_qc_hardening §G3/W1) — they consume the
        // same records, and gating the response on the diagnostic would make
        // turning the diagnostic off silently disarm the fix.  With both off the
        // block is unreached and every Card-G behaviour is byte-identical.
        // Satellites that produced a ROW-level resumption this epoch: the Card
        // GRS tier's de-duplication key (5b).  Filled only under the rider, so
        // an unarmed run pays for one empty container and nothing else.
        std::set<std::string> row_resumed;
        if (p.gap_instrument || p.gap_reentry) {
            for (const auto& r : rows) {
                const auto pit = h.prev.find(r.satellite);
                if (pit == h.prev.end()) continue;                       // first sighting
                if (pit->second.epoch_idx >= epoch_idx - 1) continue;    // adjacent (or same) — not a gap
                if (p.gap_reentry_per_signal) row_resumed.insert(r.satellite);
                GapResumption g;
                g.station    = i;
                g.satellite  = r.satellite;
                g.sig1       = r.sig1;
                g.sig2       = r.sig2;
                g.gap_epochs = epoch_idx - pit->second.epoch_idx - 1;
                g.measurable = pit->second.obs.dual && r.dual;
                // The RETURN epoch's receiver verdict, carried alongside the
                // level evidence: it is the only evidence an unmeasurable
                // resumption has, and the response's second gate.
                g.lli1       = r.lli1;
                g.lli2       = r.lli2;
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

        // ---- (5b) Card GRS: SIGNALS resuming while the ROW survived --------
        // The row-level tier at (5) keys on the satellite, and a satellite that
        // keeps ONE tracking signal keeps its row: a receiver losing lock on
        // the semi-codeless L1/L2 phases while the civil L5 tracks through
        // re-enters with an adjacent predecessor, so (5) sees nothing, no
        // time-difference detector covers the return epoch, and the carried
        // ambiguities of the returning phases fold across the break.  This tier
        // keys on the SIGNAL instead.  Satellites already reported at (5) are
        // skipped, so one break is counted once, by the tier that can see it.
        //
        // ⚠ The row-level card is a HARD prerequisite, not a preference: the
        // de-duplication list `row_resumed` is filled at (5), so with (5)
        // unreached a whole-satellite gap would be reported by BOTH tiers and
        // answered twice.  The loader warns; here the tier simply does not run.
        if (p.gap_reentry_per_signal && (p.gap_instrument || p.gap_reentry)) {
            const double gate_gf_sig = std::max(p.k_gf * s_gf, p.gf_floor_m);
            for (const auto& r : rows) {
                if (row_resumed.count(r.satellite)) continue;   // (5) owns this break
                const auto sit = h.sig_prev.find(r.satellite);
                if (sit == h.sig_prev.end()) continue;          // first sighting of the satellite
                const auto& was = sit->second;
                // This epoch's levels by signal (the partner lookup's other half).
                std::map<std::string, const RawSigObs*> now;
                for (const auto& s : r.sigs) now[s.sig] = &s;
                for (const auto& s : r.sigs) {
                    const auto wit = was.find(s.sig);
                    if (wit == was.end()) continue;                       // first sighting of the signal
                    if (wit->second.last_epoch >= epoch_idx - 1) continue; // adjacent, not a gap
                    SigResumption g;
                    g.station    = i;
                    g.satellite  = r.satellite;
                    g.signal     = s.sig;
                    g.gap_epochs = epoch_idx - wit->second.last_epoch - 1;
                    g.lli        = s.lli;
                    g.gate_gf    = gate_gf_sig;
                    // --- the witness, in the card's priority order -----------
                    // FIRST CHOICE: a partner that TRACKED THROUGH the gap,
                    // continuously present from at or before the candidate's
                    // last-seen epoch right up to now.  Its ambiguity is then
                    // known constant across the break, so the differenced level
                    // isolates the CANDIDATE's own step.  SECOND CHOICE: any
                    // partner with a snapshot that is present now; it may carry
                    // its own break, which is why that case is counted
                    // separately rather than silently graded the same way.
                    // std::map iteration is sorted, so both choices are
                    // deterministic (the lexically lowest qualifying partner).
                    const std::string* best = nullptr;
                    bool best_through = false;
                    for (const auto& kv : wit->second.gf_snap) {
                        if (!now.count(kv.first)) continue;               // not present now
                        const auto oit = was.find(kv.first);
                        const bool through = oit != was.end()
                                          && oit->second.run_start >= 0
                                          && oit->second.run_start <= wit->second.last_epoch
                                          && oit->second.last_epoch == epoch_idx - 1;
                        if (through) { best = &kv.first; best_through = true; break; }
                        if (best == nullptr) best = &kv.first;
                    }
                    if (best != nullptr) {
                        g.partner         = *best;
                        g.partner_through = best_through;
                        g.measurable      = true;
                        // d_gf = (this epoch's GF difference against the partner)
                        //        − (the same difference at the candidate's own
                        //           last-seen epoch).
                        g.d_gf = (s.phase_m - now.at(*best)->phase_m)
                               - wit->second.gf_snap.at(*best);
                        g.shifted = (gate_gf_sig > 0.0)
                                 && (std::abs(g.d_gf) > gate_gf_sig);
                    }
                    ++cnt.gap_sig_resumptions;
                    if (!g.measurable)       ++cnt.gap_sig_unmeasurable;
                    else if (g.shifted)      ++cnt.gap_sig_shifted;
                    if (g.measurable && !g.partner_through)
                        ++cnt.gap_sig_partner_broken;
                    ep.sig_resumptions.push_back(std::move(g));
                }
            }
        }

        // ---- (6) Advance the history -------------------------------------
        for (const auto& r : rows) {
            auto& slot = h.prev[r.satellite];
            slot.obs       = r;
            slot.epoch_idx = epoch_idx;
        }
        // Card GRS: …and the per-signal levels.  A signal whose predecessor is
        // not the immediately-preceding processed epoch starts a NEW continuous
        // run, which is the fact the partner test reads.  The snapshot is rebuilt
        // epoch against every OTHER signal present now, so a re-entry always
        // differences a level taken at its own last-seen epoch.  Advanced under
        // the same prerequisite the detection has, so the two never disagree
        // about which epochs the history covers.
        if (p.gap_reentry_per_signal && (p.gap_instrument || p.gap_reentry)) {
            for (const auto& r : rows) {
                if (r.sigs.empty()) continue;
                auto& smap = h.sig_prev[r.satellite];
                for (const auto& s : r.sigs) {
                    auto& rec = smap[s.sig];
                    if (rec.last_epoch < 0 || rec.last_epoch != epoch_idx - 1)
                        rec.run_start = epoch_idx;
                    rec.last_epoch = epoch_idx;
                    rec.phase_m    = s.phase_m;
                    rec.lli        = s.lli;
                    rec.gf_snap.clear();
                    for (const auto& o : r.sigs)
                        if (o.sig != s.sig) rec.gf_snap[o.sig] = s.phase_m - o.phase_m;
                }
            }
        }
    }
}

} // namespace GnssProcessor
