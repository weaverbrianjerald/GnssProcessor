/**
 * @file RawObservables.cpp
 * @brief Shared raw cycle-slip observables and the empirical qualification pass
 *        (slip-QC hardening).
 *
 * @ref networkComputeRawObservables carries the arithmetic of the
 * `network.trace_slip_raw` writer in ApplicationNetworkAssembly.cpp unchanged:
 * the same signal selection, sort, formulas and skip conditions.  When the trace
 * is the only consumer, its rows are byte-identical to the former inline
 * computation.  The qualification pass and the helpers after it have no
 * counterpart in the trace writer.
 */

#include "network/slip/RawObservables.hpp"

#include "common/GnssIdentity.hpp"

#include <algorithm>
#include <cmath>

namespace GnssProcessor {

namespace {

/// True when the observation carries at least one phase signal.  This defines the
/// denominator of the LLI fraction: a satellite with no phase cannot assert a
/// phase loss of lock, so counting it would dilute the fraction of a station whose
/// receiver sets LLI too often.
bool hasAnyPhase(const PppObservation& obs) {
    for (const auto& kv : obs.raw_observed)
        if (obsTypeOf(kv.first) == ObsType::Phase) return true;
    return false;
}

/// True when the observation sets RINEX LLI bit 0 on any phase signal.
bool assertsLli(const PppObservation& obs) {
    for (const auto& [code, lli] : obs.lli_flags)
        if (obsTypeOf(code) == ObsType::Phase && (lli & 1)) return true;
    return false;
}

}  // namespace

// =====================================================================
// Raw observable computation (verbatim from the former trace writer)
// =====================================================================

namespace {

/// One phase signal of a satellite that the tracker can use this epoch (its raw phase
/// and the code of the same signal are both reported), parsed once.
struct PhaseSignal {
    std::string phase;    ///< The phase code, e.g. "L1C".
    SignalId    signal;   ///< Its signal (band digit and attribute), e.g. 1C.
    std::string code;     ///< The code of the same signal, e.g. "C1C".
};

/// Returns the phase signal that this station latched on @p band, among the signals
/// @p usable the satellite reports this epoch, or null.
///
/// The source is the station's canonical latch, not the epoch's admitted row set
/// (@c PppObservation::observed).  The admitted set is decided per epoch, and the
/// admission and readmit logic can withhold a returning arc's phase from it for the
/// arc's first epochs, which are the re-entry epochs this tracker has to name
/// correctly.  The latch is decided once per constellation and then fixed.  A
/// latched signal is usable only when both its raw phase and its matching code are
/// reported this epoch, since the observables are built from them: exactly the
/// signals of @p usable.
const PhaseSignal* latchedPhaseOfBand(const std::vector<PhaseSignal>& usable, char band,
                                      int station, const std::string& system,
                                      const TrackerLatchLookup* latch, bool* named = nullptr) {
    if (named != nullptr) *named = false;
    if (latch == nullptr || !*latch) return nullptr;
    const std::string k = (*latch)(station, system, band);   // a phase on this band, e.g. L2W
    if (named != nullptr) *named = !k.empty();
    for (const PhaseSignal& p : usable)
        if (p.phase == k && p.signal.digit == band) return &p;
    return nullptr;
}

}  // namespace

void networkComputeRawObservables(const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  int n_sta_total,
                                  PppUtils& utils,
                                  RawSlipEpoch& out,
                                  bool per_signal,
                                  bool tracker_modeled_first,
                                  SlipQcCounters* cnt,
                                  const TrackerLatchLookup* latch,
                                  bool band_role_only) {
    for (int i = 0; i < n_sta_total; ++i) {
        if (i >= static_cast<int>(out.rows.size())) break;
        if (!present[i]) continue;
        auto& dst = out.rows[static_cast<std::size_t>(i)];
        for (const auto& obs : modeled[static_cast<std::size_t>(i)]) {
            std::vector<PhaseSignal> ph;   // the phase signals with a matching code
            for (const auto& kv : obs.raw_observed) {
                const auto phase = ObservableId::parse(obs.system, kv.first);
                if (!phase || phase->type != ObsType::Phase) continue;
                std::string code = phase->signal.obsCode(ObsType::Code);
                if (obs.raw_observed.count(code))
                    ph.push_back({kv.first, phase->signal, std::move(code)});
            }
            if (ph.empty()) continue;
            // Stable: the signals arrive in ascending key order (raw_observed is a
            // std::map), and a band's lexically lowest phase has to stay first within
            // the band.  std::sort leaves equal band digits in unspecified order.
            std::stable_sort(ph.begin(), ph.end(),
                             [](const PhaseSignal& a, const PhaseSignal& b) {
                                 return a.signal.digit < b.signal.digit;   // band digit only
                             });
            // The two lowest distinct bands: the first signal, and the first signal after
            // it on another band (none when every signal shares one band).
            const PhaseSignal* p1 = &ph.front();
            const PhaseSignal* p2 = nullptr;
            for (const PhaseSignal& p : ph)
                if (p.signal.digit != p1->signal.digit) { p2 = &p; break; }
            // ---- Name each band by the latch, not lexically -------
            // The two bands are already fixed (the two lowest distinct ones);
            // this step decides only which signal of each band the tracker
            // carries.  With tracker_modeled_first off, the lexical pick above
            // stands, so the pair, the trace rows and every detection are
            // byte-identical.  With it on, a band whose latched phase is tracked
            // this epoch takes that signal, which is the signal the carried
            // ambiguity is keyed on, so the gap re-entry response's lookup and
            // this tracker name each band the same way.  A band with no latch,
            // or whose latched phase is not reported this epoch (band 5 on a
            // dual-frequency posture, or a station that lost that channel),
            // keeps the lexical pick.
            //
            // gap_tracker_key_mismatch is counted in both knob states and
            // compares the latch with the signal the tracker carries.  Off, that
            // is the lexical pick, so the counter sizes the population the gap
            // re-entry response cannot reach; on, it is the latched signal, so
            // the counter is 0 by construction.  One counter serves both states
            // (invariant #4).
            if ((tracker_modeled_first || cnt != nullptr) && latch != nullptr) {
                const std::string system = systemName(obs.system);
                for (const PhaseSignal** p : { &p1, &p2 }) {
                    if (*p == nullptr) continue;
                    bool named = false;
                    const PhaseSignal* m =
                        latchedPhaseOfBand(ph, (*p)->signal.digit, i, system, latch, &named);
                    if (m == nullptr) {
                        // The lookup names a signal for this band but its phase is not
                        // reported with its code this epoch.  With band_role_only the band
                        // has no tracker signal this epoch; the lexical pick never stands
                        // in for a band that has a Modelled signal.
                        if (tracker_modeled_first && band_role_only && named) *p = nullptr;
                        continue;                       // no latched cell here
                    }
                    if (tracker_modeled_first) *p = m;
                    if (cnt != nullptr && m != *p) ++cnt->gap_tracker_key_mismatch;
                }
                // A satellite whose lowest band lost its signal has no tracker row this
                // epoch (a second band that lost its signal leaves a single-band row).
                if (p1 == nullptr) continue;
            }
            const std::string& s1 = p1->phase;
            const std::string  s2 = p2 ? p2->phase : std::string{};

            const double lam1 = utils.getWavelength(obs.system, s1, obs.satellite);
            if (!(std::isfinite(lam1) && lam1 > 0.0)) continue;
            const double L1_cyc = obs.raw_observed.at(s1);
            const double P1_m   = obs.raw_observed.at(p1->code);
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
                    const double P2_m   = obs.raw_observed.at(p2->code);
                    gf = L1_m - L2_cyc * lam2;
                    mw = (L1_cyc - L2_cyc) / (inv1 - inv2)
                       - (P1_m * inv1 + P2_m * inv2) / (inv1 + inv2);
                    lli2 = obs.lli_flags.count(s2) ? obs.lli_flags.at(s2) : 0;
                    // Inputs of the CS-GF authority: the code GF and the pair's
                    // dispersive scale γ−1 (QUEUE 8b successor).  These are struct
                    // fields only; the slip_raw trace writer prints an explicit
                    // column list, so a run with the authority off stays
                    // byte-identical.
                    gf_code = P1_m - P2_m;
                    const double g = (lam2 / lam1) * (lam2 / lam1);
                    gf_denom = g - 1.0;
                    // Wide-lane wavelength: the physical unit of the MW floor
                    // (mw_floor_cyc, default 0.5 WL cycles).  Exact for FDMA as
                    // well, since both wavelengths come from the satellite-aware
                    // lookup.
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
            // ---- Per-signal view of the same observation ---------
            // Everything above summarises the satellite through one pair.  The
            // per-signal re-entry tier also needs the signals the pair leaves
            // out, because those keep the row alive across a loss of lock on the
            // primary pair.  `ph` is already the lowest-band-first list of phases
            // with a matching code, so the two views agree on which signals
            // exist.  A signal whose wavelength does not resolve is dropped
            // rather than carried in cycles, because the level test differences
            // metres.
            if (per_signal) {
                r.sigs.reserve(ph.size());
                for (const PhaseSignal& p : ph) {
                    const std::string& k = p.phase;
                    const double lam = utils.getWavelength(obs.system, k, obs.satellite);
                    if (!(std::isfinite(lam) && lam > 0.0)) continue;
                    RawSigObs s;
                    s.sig     = k;
                    s.phase_m = obs.raw_observed.at(k) * lam;
                    s.code_m  = obs.raw_observed.at(p.code);
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
    // Reset conditions (invariant #1 for this authority): lost adjacency (a gap,
    // a first sighting, or a pair change that trips the gate), an empirical MW/GF
    // step (slip class), or a state that was never initialized.  The reset
    // re-seeds from the code sample alone at the code sample's own variance, so
    // the state is code-noisy for a few epochs but carries no stale level.
    if (!adjacent || stepped || cs.n <= 0 || !std::isfinite(cs.val)) {
        if (!std::isfinite(i_code_m) || !(sigma_i_code_m > 0.0)) {
            cs.n = 0;   // nothing usable this epoch; stay uninitialized
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
    // Arming gate: with fewer than chatter_min_epochs of history the verdict does
    // not move, so one noisy epoch cannot demote a station and one quiet epoch
    // cannot restore it.
    if (count < static_cast<long>(p.chatter_min_epochs)) return credible_in;
    if (credible_in) {
        // Demote only above the high threshold: an implausibly large sustained
        // fraction of the station's tracked satellites asserting loss of lock.
        return !(frac_ewma > p.chatter_frac_hi);
    }
    // Promote back below the low threshold (hysteresis band between the two).
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
        // Only a predecessor at the immediately preceding epoch forms a step.  A
        // row whose predecessor is older is a resumption (handled at (5));
        // mixing the two would let a multi-hour gap inflate the noise scale.
        std::vector<double> mw_abs, gf_abs;
        mw_abs.reserve(rows.size());
        gf_abs.reserve(rows.size());
        for (const auto& r : rows) {
            RawSlipVerdict v;
            const auto pit = h.prev.find(r.satellite);
            if (pit != h.prev.end() && pit->second.epoch_idx == epoch_idx - 1
                && pit->second.obs.dual && r.dual) {
                v.measured = true;
                // When the pair changed, the two MW values sit on different
                // wide-lane wavelengths: d_mw is still a level change, but it has
                // no meaning in cycles of the current pair's lane.  The step is
                // flagged, not suppressed: the slip detectors use it, and a consumer that
                // converts d_mw into cycles of one lane refuses it (RawVerdict::pair_changed).
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
        // Cold start (no folded history): the prior is the robust scale of this
        // epoch's own steps, so it calibrates itself and never uses a
        // stochastic-model σ (rule 4).
        std::vector<double> mw_tmp = mw_abs, gf_tmp = gf_abs;
        const double s_mw = (h.mw_step_count > 0) ? h.mw_step_scale
                                                  : networkRobustStepScale(mw_tmp);
        const double s_gf = (h.gf_step_count > 0) ? h.gf_step_scale
                                                  : networkRobustStepScale(gf_tmp);

        // ---- (3) Threshold with the pre-update scale --------------------
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
            // Only clean survivors feed the scale, so it cannot absorb the
            // events it is meant to detect (the same rule as the IF-TD pool arm).
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

        // ---- (4b) LLI credibility -------------------------------
        // The fraction is taken over the satellites that can assert a phase LLI
        // at this station this epoch.  It is folded first, and the two-state
        // hysteresis is evaluated on the post-fold EWMA.  With the default
        // chatter_min_epochs of 1, a station that sets LLI too often from its
        // first epoch is judged on that epoch's own evidence, with no warmup.
        // Its arcs are fresh then, so the carry has nothing to protect and a
        // suppression there costs nothing.
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

        // ---- (5) Arcs resuming after a tracking break -----------
        // Armed by the instrument (gap_instrument) or by the response
        // (gap_reentry, docs/SPEC_slip_qc_hardening §G3/W1).  Both consume the
        // same records; gating the response on the instrument would let turning
        // the instrument off disarm the response without notice.  With both off,
        // the block is not reached and every gap-response output is byte-identical.
        // row_resumed holds the satellites with a row-level resumption this
        // epoch and is the de-duplication key of the per-signal tier (5b,
        // per-signal gap re-entry).  It is filled only when gap_reentry_per_signal is on, so a run
        // without it pays for one empty container.
        std::set<std::string> row_resumed;
        if (p.collectsResumptions()) {
            for (const auto& r : rows) {
                const auto pit = h.prev.find(r.satellite);
                if (pit == h.prev.end()) continue;                       // first sighting
                if (pit->second.epoch_idx >= epoch_idx - 1) continue;    // adjacent (or same) epoch: not a gap
                if (p.gap_reentry_per_signal) row_resumed.insert(r.satellite);
                GapResumption g;
                g.station    = i;
                g.satellite  = r.satellite;
                g.sig1       = r.sig1;
                g.sig2       = r.sig2;
                g.gap_epochs = epoch_idx - pit->second.epoch_idx - 1;
                g.measurable = pit->second.obs.dual && r.dual;
                // The receiver's LLI at the return epoch, carried alongside the
                // level evidence: it is the only evidence an unmeasurable
                // resumption has, and it is the response's second gate.
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

        // ---- (5b) Signals resuming while the row survived ------
        // The row-level tier at (5) keys on the satellite, and a satellite that
        // keeps one tracking signal keeps its row.  When a receiver loses lock
        // on the semi-codeless L1/L2 phases while the civil L5 tracks through,
        // the row re-enters with an adjacent predecessor: (5) sees nothing, no
        // time-difference detector covers the return epoch, and the carried
        // ambiguities of the returning phases fold across the break.  This tier
        // keys on the signal instead.  Satellites already reported at (5) are
        // skipped, so each break is counted once, by the tier that can see it.
        //
        // The row-level response is a required prerequisite: the de-duplication
        // list `row_resumed` is filled at (5), so with (5) not reached a
        // whole-satellite gap would be reported by both tiers and answered
        // twice.  The loader warns about the combination; here the tier does
        // not run.
        if (p.gap_reentry_per_signal && p.collectsResumptions()) {
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
                    // --- The witness, in priority order -----------
                    // First choice: a partner that tracked through the gap, i.e.
                    // present continuously from at or before the candidate's
                    // last-seen epoch up to the previous epoch, and present now.
                    // Its ambiguity is then constant across the break, so the
                    // differenced level isolates the candidate's own step.
                    // Second choice: any partner with a snapshot that is present
                    // now.  It may carry its own break, so that case is counted
                    // separately (gap_sig_partner_broken).  std::map iteration is
                    // sorted, so the choice is deterministic: the lexically
                    // lowest through partner, else the lexically lowest present
                    // partner.
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
        // Advance the per-signal levels as well.  A signal whose
        // predecessor is not the immediately preceding processed epoch starts a
        // new continuous run, which is what the partner test reads.  Each epoch
        // the snapshot is rebuilt against every other signal present now, so a
        // re-entry always differences a level taken at its own last-seen epoch.
        // The history advances under the same prerequisite as the detection, so
        // the two agree on which epochs the history covers.
        if (p.gap_reentry_per_signal && p.collectsResumptions()) {
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
