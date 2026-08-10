/**
 * @file NetworkSlipQc.cpp
 * @brief Serial slip-QC drains of the network run loop: the phase-leveling T0
 *        repair drain, the MW-detect / LLI refloat drain, and the IF-phase
 *        triple-difference driver.
 *
 * Decomposition INC-6 (2026-07-16) — bodies relocated VERBATIM from the
 * SB_SLIP block of ApplicationNetwork.cpp (see
 * docs/network_decomposition_addendum_2026-07-16.md).  The PASS-A parallel
 * scan that FILLS PassABuffers stays in the run TU until INC-7 (Assembly);
 * the SB_SLIP D0b timer bracket also stays there, wrapping the scan plus
 * these three calls.  Each function opens with a short alias block binding
 * the former runNetwork() local names (buffer slots, trace stream, event
 * forwarder) so the moved code is textually unchanged.
 */

#include "slip/NetworkSlipQc.hpp"

#include "config/GnssConfig.hpp"        // networkCycleSlip* getters
#include "filter/StateKeySchema.hpp"    // StateKeys::parseAmbiguityKey (refloat lambdas)
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch (se.time in log/event rows)

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace GnssProcessor {

void networkSlipDrainT0Repairs(const PassABuffers& pa, const GnssConfig& config_,
                               const std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               std::map<std::tuple<int, std::string, std::string>,
                                        LocalCarry>& local_carry,
                               const SyncedEpoch& se,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-6 migration mechanic) — the relocated body reads the
    // PASS-A buffer under its former local name.
    const auto& sta_repairs = pa.sta_repairs;
    // Phase-leveling T0 (serial, station-ordered → deterministic), applied
    // BEFORE PASS-B reads the carry to build the prior.
    //   • REPAIR  — a confident, sane-magnitude integer (frac ≤ frac_max AND
    //     |dN| ≤ dn_max): shift the carried value continuously
    //     (value_abs += dN·λ, variance unchanged, covariance-preserving).
    //   • LEAVE   — a less-confident emitted repair (frac/magnitude reject):
    //     do nothing.  This is the data-driven choice for the BIA-less network:
    //     the float wide-lane's emitted-but-low-confidence repairs are
    //     predominantly FALSE positives (e.g. a re-initialising arc reporting
    //     dN=675 at frac≈0.27), so acting on them — REFLOAT or DROP — discards
    //     a GOOD carry and measurably degrades the products, whereas leaving
    //     them matches carry-only.  The residual lock-in risk on a RARE
    //     real-but-rejected slip is bounded by phase_leveling_var_floor_m2 and
    //     the max-gap drop, and is properly resolved by Increment 3's
    //     cross-station RAIM consensus (which CAN tell a real slip from a
    //     single-station false positive — something a per-station T0 cannot).
    //     Wiring external BIA into the network MW would make the wide-lane
    //     integer trustworthy and let this become a REFLOAT.
    // The resolver's `reset_sats` is likewise NOT acted on (BIA-less, it is a
    // liberal catch-all of tens of sats/epoch).  The pivot (stations[0]) is
    // never touched (its gauge-pinned ambiguities are Increment-4 territory).
    int n_t0_repairs = 0, n_t0_left = 0;
    std::string t0_detail;
    const double t0_frac_max = config_.networkCycleSlipTurboEditFracMax();
    const int    t0_dn_max   = config_.networkCycleSlipTurboEditDnMax();
    for (int i = i_lo; i < n_sta_total; ++i) {
        if (!present[i]) continue;
        for (const auto& r : sta_repairs[i]) {
            if (r.dN == 0 || !(std::isfinite(r.lambda) && r.lambda > 0.0)) continue;
            auto it = local_carry.find(std::make_tuple(i, r.satellite, r.signal));
            if (it == local_carry.end()) continue;
            const int adn = (r.dN < 0) ? -r.dN : r.dN;
            if (r.frac <= t0_frac_max && adn <= t0_dn_max) {
                it->second.value_abs += static_cast<double>(r.dN) * r.lambda;  // REPAIR
                ++n_t0_repairs;
                if (n_t0_repairs <= 12)
                    t0_detail += fmt::format("{}{}/{}/{} dN={} f={:.2f}",
                                             t0_detail.empty() ? "" : ", ",
                                             stations[i].name, r.satellite, r.signal,
                                             r.dN, r.frac);
            } else {
                ++n_t0_left;  // LEAVE (untrusted integer — likely a float-MW false positive)
                if (n_t0_left <= 6)
                    log->debug("Network[phase-leveling T0]: TOW {:.1f} LEFT low-confidence "
                               "{}/{}/{} dN={} frac={:.2f} (untrusted; carry kept)",
                               se.time.tow, stations[i].name, r.satellite, r.signal,
                               r.dN, r.frac);
            }
        }
    }
    if (n_t0_repairs > 0 || n_t0_left > 0)
        log->info("Network[phase-leveling T0]: TOW {:.1f} applied {} repair(s) [{}]; "
                  "left {} low-confidence detection(s) untouched",
                  se.time.tow, n_t0_repairs, t0_detail, n_t0_left);
}

void networkSlipDrainMwRefloat(const PassABuffers& pa, const GnssConfig& config_,
                               std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               const std::vector<std::vector<PppObservation>>& modeled,
                               int i_lo, int n_sta_total,
                               bool carry_invariant, bool carry_warm,
                               std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                               const RawSlipEpoch& raw, const SlipQcParams& qc,
                               std::vector<RawSlipHistory>& raw_hist,
                               SlipQcCounters& qc_cnt,
                               const SyncedEpoch& se, NetworkRunTraces& traces,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_mw_refloat = pa.sta_mw_refloat;
    // Thin event forwarder — textual copy of the run-TU lambda, so the moved
    // emitEvent call sites are unchanged (same rows, same order).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // MW DETECTOR (mw_detect) → refloat: the iono-safe complement to IF-phase TD.
    // Two signals, neither of which uses the geometry-free ionosphere (so both stay
    // valid at the 30 s product rate, unlike the TurboEdit MW+GF integer REPAIR):
    //   (a) a CONFIDENT wide-lane integer jump collected in PASS A (sta_mw_refloat);
    //   (b) the receiver's own LLI loss-of-lock flag (bit 0) on a carried signal —
    //       the ground truth a receiver sets on a cycle slip / re-acquisition (this
    //       is what a real slip like G23@GAMB carries when MW/IF can't see it).
    // Either REFLOATS the affected ambiguity (marks it pending-reseed so PASS B
    // re-estimates it from this epoch's data) — never applying an integer, so a wrong
    // wide-lane integer can never bias the gauge.  Serial, station-ordered →
    // deterministic.  The refloat is meaningful only when the carry is warm; cold, the
    // detection is diagnostic (the free per-epoch estimate needs no reseed).  Pivot
    // (stations[0]) is never touched (its ambiguities are gauge-pinned).
    if (config_.networkCycleSlipMwDetect()) {
        // Card E: MATCHED (a detection landed on a live ambiguity) and REFLOATED
        // (the warm carry was actually reset) are separate facts.  Conflating
        // them is exactly what made every cold run report zero MW/LLI events
        // while still feeding the C3c funnel (limitation L3).
        struct SignalAction { bool matched{false}; bool refloated{false}; };
        auto refloat_signal = [&](int i, const std::string& sat,
                                  const std::string& signal) -> SignalAction {
            SignalAction act;
            StateManager& sm = *stations[i].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                if (parts->signal == signal) {
                    act.matched = true;
                    // Card 4 C3c: feed the DD invariant carry from epoch 1,
                    // independent of the legacy carry's warm gate (the DD carry is
                    // live throughout; pending-reseed cannot serve it — PASS-B
                    // MeasurementModel consumes+clears it before the fold).
                    if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                    // Legacy phase-leveling refloat: only meaningful once warm.
                    if (carry_warm) { sm.markAmbiguityPendingReseed(amb_key); act.refloated = true; }
                }
            }
            return act;
        };
        // Card L: the LLI qualification is armed only when the toggle is on AND
        // this epoch actually has raw evidence.  Without evidence LLI stays
        // decisive — a gate that fires on missing data would be worse than the
        // chatter it is meant to fix.
        const bool lli_qual = qc.lli_corroborate && raw.computed;
        int n_mw = 0, n_lli = 0, n_lli_supp = 0;
        std::string mw_detail;
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            // (a) confident wide-lane integer slips detected in PASS A.  A satellite
            // can slip on several signal-pairs at once (multi-freq); refloat every
            // band but count / log / emit ONCE per (station, satellite).
            std::set<std::string> mw_done;
            for (const auto& [sat, sig1, sig2] : sta_mw_refloat[i]) {
                const auto a = refloat_signal(i, sat, sig1);
                const auto b = refloat_signal(i, sat, sig2);
                const bool matched   = a.matched   || b.matched;
                const bool refloated = a.refloated || b.refloated;
                if (matched && mw_done.insert(sat).second) {
                    emitEvent("T0_mw_detect", "mw_wl_slip", sat, stations[i].name,
                              std::nullopt, std::nullopt,
                              refloated ? "refloat" : "diagnostic", sig1 + ";" + sig2);
                    ++qc_cnt.mw_detections;
                    if (refloated) ++qc_cnt.mw_refloats;
                    if (++n_mw <= 12)
                        mw_detail += fmt::format("{}{}/{}", mw_detail.empty() ? "" : ", ",
                                                 stations[i].name, sat);
                }
            }
            // (b) receiver LLI loss-of-lock (bit 0) on any carried phase signal.
            // modeled[i] holds one entry per satellite; refloat every LLI'd band but
            // count / emit ONCE per satellite (an LLI on both L1 and L2 is one slip).
            const bool credible =
                !lli_qual || (i < static_cast<int>(raw.lli_credible.size())
                              && raw.lli_credible[static_cast<std::size_t>(i)] != 0);
            for (const auto& obs : modeled[i]) {
                // Card L qualification is a per-(station, satellite) decision, so
                // establish "does this satellite assert LLI at all" before acting.
                bool asserts = false;
                for (const auto& [code, lli] : obs.lli_flags)
                    if (!code.empty() && code[0] == 'L' && (lli & 1)) { asserts = true; break; }
                if (!asserts) continue;

                bool corr_mw = false, corr_gf = false, measurable = false, offpair = false;
                if (lli_qual && i < static_cast<int>(raw.verdict.size())) {
                    const auto& vm  = raw.verdict[static_cast<std::size_t>(i)];
                    const auto  vit = vm.find(obs.satellite);
                    if (vit != vm.end() && vit->second.measured) {
                        measurable = true;
                        corr_mw    = vit->second.mw_step;
                        corr_gf    = vit->second.gf_step && !corr_mw;
                    }
                    // Scope honesty: the shared observables are formed on the
                    // PRIMARY dual-frequency pair, so an LLI asserted only on a
                    // band OUTSIDE that pair (e.g. L5 on an L1/L2-primary arc)
                    // is not something MW/GF can witness.  Counted separately so
                    // "uncorroborated" never silently absorbs "unwitnessable" —
                    // a nonzero off-pair count is the trigger to widen the
                    // corroboration primitive to every tracked pair.
                    if (measurable) {
                        bool on_pair = false;
                        for (const auto& r : raw.rows[static_cast<std::size_t>(i)]) {
                            if (r.satellite != obs.satellite) continue;
                            for (const auto& [code, lli] : obs.lli_flags)
                                if (!code.empty() && code[0] == 'L' && (lli & 1)
                                    && (code == r.sig1 || code == r.sig2)) { on_pair = true; break; }
                            break;
                        }
                        offpair = !on_pair;
                    }
                }
                const bool corroborated = corr_mw || corr_gf;
                // ACT when the assertion is corroborated by the receiver's own
                // observables (decisive at ANY station — this is what keeps a real
                // multi-satellite re-lock intact, spec M3), or when the station's
                // LLI is still credible (spec M1 — do NOT require corroboration
                // everywhere; that would forfeit the G23@GAMB class of catches
                // where MW and IF genuinely cannot see the slip).
                const bool act = !lli_qual || corroborated || credible;

                if (lli_qual) {
                    RawSlipHistory* h = (static_cast<std::size_t>(i) < raw_hist.size())
                                            ? &raw_hist[static_cast<std::size_t>(i)] : nullptr;
                    ++qc_cnt.lli_asserted;
                    if (h) ++h->lli_asserted;
                    if (corr_mw)      { ++qc_cnt.lli_corr_mw; if (h) ++h->lli_corr_mw; }
                    else if (corr_gf) { ++qc_cnt.lli_corr_gf; if (h) ++h->lli_corr_gf; }
                    if (!measurable)  { ++qc_cnt.lli_unmeasurable; if (h) ++h->lli_unmeasurable; }
                    if (offpair)      { ++qc_cnt.lli_offpair;      if (h) ++h->lli_offpair; }
                    if (!act) {
                        ++qc_cnt.lli_suppressed;
                        if (h) ++h->lli_suppressed;
                        ++n_lli_supp;
                        // M4: never silent.  The suppressed assertion is a row of
                        // its own, carrying the station's credibility score so the
                        // top-offenders list is reconstructible from the trace.
                        emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                                  stations[i].name,
                                  h ? std::optional<double>(h->lli_frac_ewma) : std::nullopt,
                                  std::optional<double>(qc.chatter_frac_hi),
                                  "suppressed",
                                  !measurable ? "LLI;uncorroborated;unmeasurable"
                                              : (offpair ? "LLI;uncorroborated;off_primary_pair"
                                                         : "LLI;uncorroborated"));
                        continue;
                    }
                    if (h) ++h->lli_acted;
                }

                bool matched = false, refloated = false;
                for (const auto& [code, lli] : obs.lli_flags) {
                    if (code.empty() || code[0] != 'L' || !(lli & 1)) continue;
                    const auto sa = refloat_signal(i, obs.satellite, code);
                    if (sa.matched)   matched   = true;
                    if (sa.refloated) refloated = true;
                }
                if (matched) {
                    emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                              stations[i].name, std::nullopt, std::nullopt,
                              refloated ? "refloat" : "diagnostic",
                              corr_mw ? "LLI;mw_step"
                                      : (corr_gf ? "LLI;gf_step" : "LLI"));
                    ++n_lli;
                    ++qc_cnt.lli_detections;
                    if (refloated) ++qc_cnt.lli_refloats;
                }
            }
        }
        if (n_mw > 0 || n_lli > 0 || n_lli_supp > 0)
            log->info("Network[MW detect]: TOW {:.1f} {} {} wide-lane slip(s){} + "
                      "{} LLI loss-of-lock(s){}", se.time.tow,
                      carry_warm ? "refloated" : "detected (carry cold; diagnostic)",
                      n_mw,
                      mw_detail.empty() ? std::string{} : (" [" + mw_detail + "]"), n_lli,
                      n_lli_supp > 0
                          ? fmt::format("; {} uncorroborated LLI assertion(s) SUPPRESSED "
                                        "at non-credible station(s)", n_lli_supp)
                          : std::string{});
    }
}

void networkSlipIfTdDriver(const PassABuffers& pa, const GnssConfig& config_,
                           std::vector<NetStation>& stations,
                           const std::vector<bool>& present,
                           int i_lo, int n_sta_total,
                           bool carry_invariant, bool carry_warm,
                           std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                           bool trace_flag_rates, FlagRate& fr_if_slip,
                           const SyncedEpoch& se, NetworkRunTraces& traces,
                           const std::shared_ptr<spdlog::logger>& log,
                           std::set<std::string>* refsat_mark_sats) {
    // Alias block (INC-6 migration mechanic).
    const auto& sta_if_resets   = pa.sta_if_resets;
    auto&       if_slip_trace   = traces.if_slip_trace;
    // Thin event forwarder — textual copy of the run-TU lambda (see above).
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    // IF-phase triple-difference slip detection + refloat (this increment).
    // Each non-pivot station's detector ran in DEFERRED mode above: it computed
    // the between-satellite, between-epoch ionosphere-free phase double
    // differences (ΔΔφ_IF) per (sat, signal-pair) and cached them, but did NOT
    // flag.  The driver decides, elevation-aware and per (station, signal-pair):
    //
    //  1. CENTER — pool ΔΔφ_IF across ALL present non-pivot stations BY SIGNAL
    //     PAIR, take the median `med` (raw m): the network common-mode (frame /
    //     datum wobble), which is elevation-independent, so it is removed in raw
    //     space.
    //  2. SCALE — normalize each residual to a zenith-equivalent, z = (dd−med)·
    //     sin(el) = (dd−med)/ifElevMap, so satellites at every elevation share one
    //     scale.  The pooled s_pool = 1.4826·median(|z|) is the warmup PRIOR; each
    //     (station, pair) also keeps an EWMA of its OWN z-scale (if_pair_scale).
    //     The effective scale shrinks from the pool toward the station's EWMA as
    //     that station-pair accumulates clean samples (empirical-Bayes, weight
    //     count/(count+κ)) — so a quiet reference station and a noisy modernized-
    //     signal station each get their OWN envelope, not one pooled threshold set
    //     by the majority (the fix for the L5Q/L2L over-firing).
    //  3. THRESHOLD sat i — max(k·s_eff·ifElevMap(el_i), floor_frac·λ_IF(1,1)):
    //     the empirical term re-inflated to sat i's elevation, the physical
    //     (1,1)-integer floor kept in raw m so a real slip is caught at ANY
    //     elevation.  Flag if |dd_i − med| exceeds it.
    //  4. A flagged arc has its detector gradient reset (fresh arc next epoch —
    //     arc hygiene, every epoch) and, when the carry is active, BOTH band
    //     ambiguities marked pending-reseed so PASS B refloats them.  Clean
    //     survivors then update the station-pair EWMA (slips excluded, so the
    //     scale never absorbs the event it must catch).
    //
    // The detector's own reference-satellite common-mode slip keys (which the
    // per-pair pool cannot see) are folded in too.  Thresholds are deliberately
    // loose; the sharp pass is a future post-estimation phase validation, not yet
    // built.  Serial, station-ordered → deterministic.  Pivot has no detector.
    if (config_.networkCycleSlipIfPhaseTd()) {
        const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
        const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
        const int    min_pool     = config_.networkCycleSlipIfPhaseTdMinPool();
        const double ewma_alpha   = config_.networkCycleSlipIfPhaseTdEwmaAlpha();
        const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

        // Refloat both band ambiguities of (sat, sig1, sig2) at station i (only
        // when the carry is active; cold, the detection is diagnostic only).
        auto refloat_pair = [&](int i, const std::string& sat,
                                const std::string& sig1, const std::string& sig2) {
            StateManager& sm = *stations[i].sm;
            for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                if (!parts || parts->satellite != sat) continue;
                if (parts->signal == sig1 || parts->signal == sig2) {
                    // Card 4 C3c: feed the DD carry from epoch 1 (see refloat_signal).
                    if (carry_invariant) carry_slipped_ambs.insert({i, amb_key});
                    if (carry_warm) sm.markAmbiguityPendingReseed(amb_key);
                }
            }
        };

        // Pool this epoch's ΔΔφ_IF by signal pair across present non-pivot stations.
        // Carry elevation (for the stochastic normalization) and the per-sat (1,1)
        // IF wavelength (for the physical floor — exact even for GLONASS FDMA).
        struct PairSample { int sta; std::string sat, sig1, sig2; double dd, el, lambda; };
        std::map<std::string, std::vector<PairSample>> by_pair;
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i] || !stations[i].if_slip) continue;
            for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                if (jp.sig1.empty() || jp.sig2.empty()) continue;
                const std::string pair = jp.sig1 + "_" + jp.sig2;
                by_pair[pair].push_back({i, sat, jp.sig1, jp.sig2,
                                         jp.delta_grad_m, jp.elevation_deg, jp.lambda_if_m});
            }
        }

        int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;
        std::string if_detail;
        for (auto& [pair, samples] : by_pair) {
            // (1) Network common-mode center: median of the raw DDs (m).
            std::vector<double> raw;
            raw.reserve(samples.size());
            for (const auto& s : samples) raw.push_back(s.dd);
            std::nth_element(raw.begin(), raw.begin() + raw.size() / 2, raw.end());
            const double med = raw[raw.size() / 2];

            // (2) Pooled elevation-normalized robust scale (zenith-equiv m):
            //     s_pool = 1.4826·median(|z|), z = (dd−med)/ifElevMap(el).
            std::vector<double> zabs;
            zabs.reserve(samples.size());
            for (const auto& s : samples)
                zabs.push_back(std::abs((s.dd - med) / ifElevMap(s.el)));
            std::nth_element(zabs.begin(), zabs.begin() + zabs.size() / 2, zabs.end());
            const double s_pool = 1.4826 * zabs[zabs.size() / 2];
            if (static_cast<int>(samples.size()) < min_pool) ++n_sparse;

            // Group the pair's samples by station (ordered → deterministic) so each
            // station-pair thresholds with its OWN shrunk scale, then updates its
            // OWN EWMA once this epoch's flags are known.
            std::map<int, std::vector<const PairSample*>> by_sta;
            for (const auto& s : samples) by_sta[s.sta].push_back(&s);

            for (auto& [sta, plist] : by_sta) {
                auto& sc = stations[sta].if_pair_scale[pair];

                // Empirical-Bayes shrinkage: pool prior (cold) → station EWMA (warm).
                const double w = static_cast<double>(sc.count) /
                                 (static_cast<double>(sc.count) + shrink_kappa);
                double s_eff = (sc.count > 0) ? (w * sc.ewma_scale + (1.0 - w) * s_pool)
                                              : s_pool;
                if (!(s_eff > 0.0)) s_eff = s_pool;  // degenerate all-equal pool
                if (sc.count >= static_cast<long>(shrink_kappa)) ++n_warm_sp;

                std::vector<double> clean_zabs;
                for (const PairSample* sp : plist) {
                    const double emap    = ifElevMap(sp->el);
                    const double floor_m = floor_frac * sp->lambda;
                    const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                    const double abs_dev = std::abs(sp->dd - med);
                    const bool   flagged = (abs_dev > thr);
                    if (trace_flag_rates) { ++fr_if_slip.opp; if (flagged) ++fr_if_slip.flag; }  // B8/T1
                    if (flagged) {
                        stations[sp->sta].if_slip->resetSatellite(sp->sat);  // restart arc
                        refloat_pair(sp->sta, sp->sat, sp->sig1, sp->sig2);
                        emitEvent("T1_if_slip", "if_slip", sp->sat,
                                  stations[sp->sta].name, abs_dev, thr,
                                  carry_warm ? "refloat" : "diagnostic",
                                  sp->sig1 + ";" + sp->sig2);
                        ++n_if_arcs;
                        if (n_if_arcs <= 12)
                            if_detail += fmt::format("{}{}/{}_{}_{}", if_detail.empty() ? "" : ", ",
                                                     stations[sp->sta].name, sp->sat, sp->sig1, sp->sig2);
                    } else {
                        clean_zabs.push_back(abs_dev / emap);
                    }
                    // Deep-investigation row: the full decision context for this
                    // (station, sat, pair) sample.  sc.ewma_scale / sc.count are the
                    // decision-time (pre-update) values used to form s_eff.
                    if (if_slip_trace.is_open())
                        if_slip_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << stations[sp->sta].name << ',' << sp->sat << ','
                            << sp->sig1 << ',' << sp->sig2 << ','
                            << fmt::format("{:.2f}", sp->el) << ','
                            << fmt::format("{:.6f}", sp->dd) << ',' << fmt::format("{:.6f}", med) << ','
                            << fmt::format("{:.6f}", s_pool) << ',' << fmt::format("{:.6f}", sc.ewma_scale) << ','
                            << sc.count << ',' << fmt::format("{:.3f}", w) << ','
                            << fmt::format("{:.6f}", s_eff) << ',' << fmt::format("{:.3f}", emap) << ','
                            << fmt::format("{:.6f}", thr) << ',' << fmt::format("{:.6f}", floor_m) << ','
                            << fmt::format("{:.6f}", abs_dev) << ',' << (flagged ? 1 : 0) << ','
                            << (carry_warm ? 1 : 0) << '\n';
                }

                // (4) Fold clean survivors into the station-pair EWMA (slips excluded).
                if (!clean_zabs.empty()) {
                    std::nth_element(clean_zabs.begin(),
                                     clean_zabs.begin() + clean_zabs.size() / 2, clean_zabs.end());
                    const double s_epoch = 1.4826 * clean_zabs[clean_zabs.size() / 2];
                    sc.ewma_scale = (sc.count == 0)
                        ? s_epoch
                        : (1.0 - ewma_alpha) * sc.ewma_scale + ewma_alpha * s_epoch;
                    sc.count += static_cast<long>(clean_zabs.size());
                }
            }
        }

        // Reference-satellite common-mode slips (per station; pool can't see them).
        for (int i = i_lo; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& rkey : sta_if_resets[i]) {
                std::string sat, sig1, sig2;
                if (!parseIfPairResetKey(rkey, sat, sig1, sig2)) continue;
                refloat_pair(i, sat, sig1, sig2);
                emitEvent("T1_if_slip", "refsat_reset", sat, stations[i].name,
                          std::nullopt, std::nullopt,
                          carry_warm ? "refloat" : "diagnostic", sig1 + ";" + sig2);
                // Edge-1 piece 2 (network.mark_refsat_reset_disc; nullptr ⇒ OFF ⇒
                // byte-identical).  Queue the satellite for a per-sat discontinuity
                // mark at its next actual emit.  Gated on carry_warm for the same
                // reason the event's action field is: while the carry is cold the
                // detection is DIAGNOSTIC — no refloat happened, so there is no
                // datum re-reference for a consumer to react to, and marking one
                // would be a fabricated event.
                if (refsat_mark_sats && carry_warm) refsat_mark_sats->insert(sat);
                ++n_if_arcs;
                if (n_if_arcs <= 12)
                    if_detail += fmt::format("{}{}/{} (refsat)", if_detail.empty() ? "" : ", ",
                                             stations[i].name, rkey);
            }
        }

        if (n_if_arcs > 0)
            log->info("Network[IF-phase slip]: TOW {:.1f} flagged {} arc(s){} across {} pair(s) "
                      "({} station-pair(s) warm){}{}",
                      se.time.tow, n_if_arcs,
                      carry_warm ? " for refloat" : " (carry cold; diagnostic)",
                      by_pair.size(), n_warm_sp,
                      n_sparse > 0 ? fmt::format(", {} pair(s) under min_pool({})", n_sparse, min_pool)
                                   : std::string{},
                      if_detail.empty() ? std::string{} : (" [" + if_detail + "]"));
    }
}

} // namespace GnssProcessor
