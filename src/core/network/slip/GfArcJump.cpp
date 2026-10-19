/**
 * @file GfArcJump.cpp
 * @brief The arc-scoped geometry-free jump trigger (slip-ladder defect 1): the
 *        per-arc test, the PASS-A drain that answers its flags, and its run-end report.
 *
 * The design (what the test is, why its reference is the last clean step, what a flag
 * frees) is in the header's file banner, include/network/slip/GfArcJump.hpp.
 */

#include "network/slip/GfArcJump.hpp"

#include "network/driver/SetupResult.hpp"   // NetworkSetupResult::stations (run-end names)
#include "common/GnssIdentity.hpp"       // LosId (RawSlipEpoch::gf_jump_flagged)
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"      // NetworkRunEndView
#include "network/driver/RunGroups.hpp"   // NetworkRunEndInputs::engines.slip_qc
#include "network/state/StationParams.hpp"   // the station's parameter set: the ambiguities of the pair
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch
#include "network/slip/SlipQc.hpp"        // NetStation, RelevelEpochState, RelevelPend, SlipQcChannel, commonBandMayFree, NetworkRunTraces

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

// =====================================================================
// The per-arc test (pure)
// =====================================================================

double gfArcJumpRobustScale(const std::deque<double>& r) {
    if (r.empty()) return 0.0;
    std::vector<double> v(r.begin(), r.end());
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    const double med = v[mid];
    for (double& x : v) x = std::abs(x - med);
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    return 1.4826 * v[mid];
}

GfArcJumpVerdict gfArcJumpStep(GfArcJumpArc& arc, long long epoch_idx, double t_s,
                               const std::string& sig1, const std::string& sig2,
                               double gf_m, const GfArcJumpParams& p) {
    GfArcJumpVerdict v;
    const bool   had_sample = arc.last_epoch >= 0;
    const double span_s     = t_s - arc.last_t_s;

    // (1) Continuity: the previous processed epoch, the same primary pair, and (once a
    //     reference exists) the reference step's span.  Anything else restarts the arc
    //     at this sample, which then opens it.
    const bool continuous = had_sample
                         && epoch_idx == arc.last_epoch + 1
                         && arc.sig1 == sig1 && arc.sig2 == sig2
                         && std::isfinite(gf_m)
                         && (!arc.has_ref
                             || std::abs(span_s - arc.ref_span_s) <= kGfArcJumpSpanTolS);
    if (!continuous) {
        v.restarted = had_sample;
        arc = GfArcJumpArc{};
        // A non-finite sample cannot open an arc: the arc stays empty, so the next
        // finite sample opens it instead of stepping from a NaN level.
        if (!std::isfinite(gf_m)) return v;
        arc.last_epoch = epoch_idx;
        arc.last_t_s   = t_s;
        arc.sig1       = sig1;
        arc.sig2       = sig2;
        arc.gf_m       = gf_m;
        return v;
    }

    const double step_m = gf_m - arc.gf_m;
    arc.last_epoch = epoch_idx;
    arc.last_t_s   = t_s;
    arc.gf_m       = gf_m;
    v.step_m       = step_m;

    // (2) The arc's first step (or the first after a re-seed) is the reference: there
    //     is nothing to compare it with.
    if (!arc.has_ref) {
        arc.has_ref    = true;
        arc.ref_step_m = step_m;
        arc.ref_span_s = span_s;
        arc.flag_run   = 0;
        return v;
    }

    // (3) Test against the last clean step, with the threshold formed before this
    //     sample can enter the scale.
    v.tested     = true;
    v.ref_step_m = arc.ref_step_m;
    v.r_m        = step_m - arc.ref_step_m;
    const int min_count = std::max(1, p.min_count);
    if (p.k > 0.0 && static_cast<int>(arc.clean_r.size()) >= min_count) {
        v.scale_m = gfArcJumpRobustScale(arc.clean_r);
        v.n_clean = static_cast<int>(arc.clean_r.size());
    }
    v.thr_m   = std::max(p.floor_m, p.k * v.scale_m);
    v.flagged = std::abs(v.r_m) > v.thr_m;

    // (4) A flagged step never becomes the reference or enters the scale; a full flag
    //     run re-seeds the reference from the next step.  A clean step does both.
    if (v.flagged) {
        if (++arc.flag_run >= kGfArcJumpMaxFlagRun) {
            v.reseeded   = true;
            arc.has_ref  = false;
            arc.flag_run = 0;
        }
    } else {
        arc.flag_run   = 0;
        arc.ref_step_m = step_m;
        arc.clean_r.push_back(v.r_m);
        const std::size_t cap = static_cast<std::size_t>(std::max(1, p.window));
        while (arc.clean_r.size() > cap) arc.clean_r.pop_front();
    }
    return v;
}

// =====================================================================
// The PASS-A drain
// =====================================================================

namespace {

/// What answering one flag did to the pair's ambiguities.
struct GfJumpAction {
    int         matched{0};     ///< Live ambiguities of the pair found.
    int         freed{0};       ///< ...freed by this drain.
    int         coincident{0};  ///< ...already freed this epoch by an earlier drain (cause bit added).
    int         cb_kept{0};     ///< ...kept pinned by the common-band verdict.
    std::string cb_kept_sigs;   ///< Their signals, '+'-joined, for the event row.
};

/// Free the live ambiguities of (@p i, @p sat) on @p sig1 / @p sig2, as the IF-TD pair
/// refloat does (header banner, "The response").
GfJumpAction gfJumpFreePair(SlipQcChannel& qc, NetStation& st, int i,
                            const std::string& sat, const std::string& sig1,
                            const std::string& sig2, bool carry_invariant, bool carry_warm,
                            std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            RelevelEpochState* relevel,
                            std::map<std::tuple<int, std::string, std::string>,
                                     RelevelPend>* relevel_pend,
                            const CommonBandEpoch* common_band) {
    GfJumpAction act;
    StateManager& sm = *st.sm;   // the re-seed marks stay in the state manager
    for (const auto& [arc, slot] : st.params.amb) {
        (void)slot;
        if (net::satKey(arc.sat) != sat) continue;
        const std::string arc_signal = net::signalKey(arc.signal);
        if (arc_signal != sig1 && arc_signal != sig2) continue;
        const std::string amb_key = net::ambKey(arc);
        ++act.matched;
        if (!commonBandMayFree(common_band, i, sat, arc_signal)) {
            ++act.cb_kept;
            if (!act.cb_kept_sigs.empty()) act.cb_kept_sigs += "+";
            act.cb_kept_sigs += arc_signal;
            continue;
        }
        const std::pair<int, std::string> key{i, amb_key};
        // Already freed this epoch by an earlier drain: record that this tier saw it
        // too, and leave the response (and any pend that drain created) as it was.
        if (relevel && relevel->free_ambs.count(key)) {
            relevel->free_cause[key] |= RelevelEpochState::kCauseGfJump;
            ++act.coincident;
            continue;
        }
        // The DD invariant carry owns its own funnel and is live from epoch 1,
        // as at every other freeing site.
        if (carry_invariant) carry_slipped_ambs.insert(key);
        if (!carry_warm) continue;
        sm.markAmbiguityPendingReseed(amb_key);
        ++act.freed;
        if (relevel) {
            relevel->free_ambs.insert(key);
            relevel->free_cause[key] |= RelevelEpochState::kCauseGfJump;
            ++qc.counters.relevel_free_marked;
        }
    }
    // A fresh mark makes the pre-slip capture of any outstanding bridge pend of this
    // (station, satellite) stale (the rule every freeing drain applies).
    if (act.freed > 0 && relevel_pend)
        for (auto pit = relevel_pend->begin(); pit != relevel_pend->end();)
            if (std::get<0>(pit->first) == i && std::get<1>(pit->first) == sat) {
                ++qc.counters.bridge_pend_voided;
                pit = relevel_pend->erase(pit);
            } else {
                ++pit;
            }
    return act;
}

/// Keep the largest |r| and its identity.
void gfJumpTrackMax(double abs_r, double& max_m, std::string& max_id,
                    const std::string& station, const std::string& sat, double tow,
                    double el_deg) {
    if (!(abs_r > max_m)) return;
    max_m  = abs_r;
    max_id = fmt::format("{}|{}@{:.0f}|el={:.1f}", station, sat, tow, el_deg);
}

}  // namespace

void networkSlipDrainGfArcJump(SlipQcChannel& qc,
                               std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               bool carry_invariant, bool carry_warm,
                               std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                               const SyncedEpoch& se, NetworkRunTraces& traces,
                               const std::shared_ptr<spdlog::logger>& log,
                               RelevelEpochState* relevel,
                               std::map<std::tuple<int, std::string, std::string>,
                                        RelevelPend>* relevel_pend,
                               const CommonBandEpoch* common_band) {
    const GfArcJumpParams& p = qc.params.gf_jump;
    if (!p.armed || !qc.epoch.computed) return;
    if (qc.hist.size() < qc.epoch.rows.size()) qc.hist.resize(qc.epoch.rows.size());
    GfArcJumpCounters& C = qc.counters.gf_jump;
    const double now_s = static_cast<double>(se.time.week) * 604800.0 + se.time.tow;

    int n_flag = 0;
    std::string log_detail;
    const int n_rows = std::min(n_sta_total, static_cast<int>(qc.epoch.rows.size()));
    for (int i = 0; i < n_rows && i < static_cast<int>(stations.size()); ++i) {
        if (i < static_cast<int>(present.size()) && !present[i]) continue;
        RawSlipHistory& h = qc.hist[static_cast<std::size_t>(i)];
        const std::string& sta_name = stations[static_cast<std::size_t>(i)].name;
        for (const RawSlipObs& row : qc.epoch.rows[static_cast<std::size_t>(i)]) {
            if (!row.dual) continue;
            ++C.samples;
            GfArcJumpArc& arc = h.gf_arcs[row.satellite];
            const GfArcJumpVerdict v =
                gfArcJumpStep(arc, qc.epoch_idx, now_s, row.sig1, row.sig2, row.gf_m, p);
            if (v.restarted) ++C.restarts;
            if (v.reseeded)  ++C.reseeds;
            if (!v.tested) continue;
            ++C.tests;
            ++h.gf_jump_tests;
            if (v.n_clean > 0) {
                ++C.scale_tests;
                if (p.k * v.scale_m > p.floor_m) ++C.scale_bound;
            }
            const double abs_r = std::abs(v.r_m);
            gfJumpTrackMax(abs_r, C.max_abs_r_m, C.max_abs_r_id, sta_name, row.satellite,
                           se.time.tow, row.elev_deg);
            if (!v.flagged) {
                gfJumpTrackMax(abs_r, C.max_clean_r_m, C.max_clean_r_id, sta_name,
                               row.satellite, se.time.tow, row.elev_deg);
                continue;
            }

            // A flag: answer it.
            ++C.flags;
            ++h.gf_jump_flags;
            if (const auto sat_id = SatId::parse(row.satellite))
                qc.epoch.gf_jump_flagged.insert(LosId{StationId{i}, *sat_id});
            const char* action = "diagnostic";
            GfJumpAction act;
            if (i < i_lo) {
                // A gauge-pinned station index (legacy pivot datum): its ambiguities are
                // never refloated locally.
                ++C.flags_pivot;
                action = "pivot";
            } else {
                act = gfJumpFreePair(qc, stations[static_cast<std::size_t>(i)], i,
                                     row.satellite, row.sig1, row.sig2, carry_invariant,
                                     carry_warm, carry_slipped_ambs, relevel, relevel_pend,
                                     common_band);
                C.bands_freed      += act.freed;
                C.bands_coincident += act.coincident;
                C.bands_cb_kept    += act.cb_kept;
                if (!carry_warm)             { ++C.flags_cold;       action = "diagnostic"; }
                else if (act.matched == 0)   { ++C.flags_no_arc;     action = "no_arc"; }
                else if (act.freed > 0)      { ++C.flags_freed;      action = "refloat"; }
                else if (act.coincident > 0) { ++C.flags_coincident; action = "coincident"; }
                else                         { ++C.flags_cb_kept;    action = "cb_kept"; }
            }
            traces.emitEvent(se.time, "T1_gf_jump", "gf_jump", row.satellite, sta_name,
                             abs_r, v.thr_m, action,
                             fmt::format("{};{};r={:+.4f};step={:+.4f};ref={:+.4f};"
                                         "scale={:.4f};n={};el={:.1f}",
                                         row.sig1, row.sig2, v.r_m, v.step_m,
                                         v.ref_step_m, v.scale_m, v.n_clean, row.elev_deg)
                                 + (act.cb_kept_sigs.empty()
                                        ? std::string{}
                                        : ";cb_kept=" + act.cb_kept_sigs));
            if (++n_flag <= 12)
                log_detail += fmt::format("{}{}/{} r={:+.3f}", log_detail.empty() ? "" : ", ",
                                          sta_name, row.satellite, v.r_m);
        }
    }
    if (n_flag > 0)
        log->info("Network[GF arc jump]: TOW {:.1f} flagged {} arc(s){} [{}]",
                  se.time.tow, n_flag,
                  carry_warm ? " for refloat" : " (carry cold; diagnostic)", log_detail);
}

// =====================================================================
// Run-end report
// =====================================================================

int networkGfArcJumpRunEnd(const NetworkRunEndView& v) {
    const SlipQcChannel& slip_qc = v.run.engines.slip_qc;
    const GfArcJumpParams& p = slip_qc.params.gf_jump;
    if (!p.armed) return 0;
    const GfArcJumpCounters& C = slip_qc.counters.gf_jump;
    const auto& stations = v.setup.stations;

    v.log->info("Network (GF arc jump): {} test(s) over {} dual-band sample(s); "
                "{} flag(s) ({:.2e} per test): {} freed ({} band(s)), {} coincident with an "
                "earlier drain ({} band(s)), {} kept whole by the common-band verdict ({} "
                "band(s)), {} with no live arc, {} cold (diagnostic), {} at a pinned station.  "
                "Bar max({:.3f} m, {}*1.4826*MAD) over {} clean value(s) once {} are held: the "
                "scale was active on {} test(s) and set the bar on {}.  {} arc restart(s), {} "
                "reference re-seed(s) after {} consecutive flags.  Largest |r| {:.4f} m at {}; "
                "largest unflagged |r| {:.4f} m at {}.",
                C.tests, C.samples, C.flags,
                C.tests > 0 ? static_cast<double>(C.flags) / static_cast<double>(C.tests) : 0.0,
                C.flags_freed, C.bands_freed, C.flags_coincident, C.bands_coincident,
                C.flags_cb_kept, C.bands_cb_kept, C.flags_no_arc, C.flags_cold,
                C.flags_pivot, p.floor_m, p.k, p.window, p.min_count,
                C.scale_tests, C.scale_bound, C.restarts, C.reseeds, kGfArcJumpMaxFlagRun,
                C.max_abs_r_m, C.max_abs_r_id.empty() ? std::string("none") : C.max_abs_r_id,
                C.max_clean_r_m,
                C.max_clean_r_id.empty() ? std::string("none") : C.max_clean_r_id);

    // The stations with the most flags (a false-flag rate concentrates on a few
    // receivers, so the list is what a field arm is read by).
    std::vector<std::pair<long long, std::size_t>> by_sta;
    for (std::size_t i = 0; i < slip_qc.hist.size(); ++i)
        if (slip_qc.hist[i].gf_jump_flags > 0)
            by_sta.emplace_back(slip_qc.hist[i].gf_jump_flags, i);
    std::stable_sort(by_sta.begin(), by_sta.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string top;
    for (std::size_t k = 0; k < by_sta.size() && k < 10; ++k) {
        const std::size_t i = by_sta[k].second;
        const std::string name = i < stations.size() ? stations[i].name : fmt::format("#{}", i);
        top += fmt::format("{}{} {}/{}", top.empty() ? "" : ", ", name, by_sta[k].first,
                           slip_qc.hist[i].gf_jump_tests);
    }
    v.log->info("Network (GF arc jump): {} station(s) flagged; top (flags/tests): {}.",
                by_sta.size(), top.empty() ? std::string("none") : top);

    if (C.tests == 0)
        v.log->warn("Network (GF arc jump): network.cycle_slip.gf_arc_jump is armed "
                    "and tested NOTHING.  The raw observables were not computed, or no arc "
                    "held three consecutive dual-band samples: the trigger is inert this run "
                    "(standing invariant 4).");
    return 0;
}

} // namespace GnssProcessor
