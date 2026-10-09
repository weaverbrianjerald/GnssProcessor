/**
 * @file NetworkClockQc.cpp
 * @brief Implementation of @ref networkClockQc, the post-solve satellite-clock QC
 *        block relocated out of Application::runNetwork at decomposition INC-10.
 *
 * The body was moved from the former SB_CLOCKUPD block of ApplicationNetwork.cpp
 * with only its indentation changed.  The alias preamble reconstructs the run-TU
 * alias block (nc, clk_pred, the knob/carry/counter/trace aliases) and the
 * emitEvent and handover_affects lambdas, so the relocated code reads the original
 * names.  See NetworkClockQc.hpp for the ordering contract.
 */

#include "filter/NetworkClockQc.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.* + networkRetroUraBookkeeping
#include "common/GnssIdentity.hpp"            // systemOfSat: the per-constellation medians
#include "config/GnssConfig.hpp"              // config_.getNetworkConfig() (nc.trace_clock_full)
#include "filter/NetworkPivotHandover.hpp"    // datumChangeAffectsKey
#include "filter/NetworkResponses.hpp"        // prmRespond / ResponseSite (card PRM phase 3a)
#include "logging/NetworkRunTraces.hpp"       // clk_mon_trace + emitEvent

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include <memory>

namespace GnssProcessor {

void networkClockQc(NetworkRunContext& ctx,
                    const GnssConfig& config_,
                    const std::shared_ptr<spdlog::logger>& log,
                    NetworkRunTraces& traces,
                    const EpochWorkspace& ws,
                    const SyncedEpoch& se,
                    const NetworkEstimator& est,
                    const Eigen::VectorXd& x_s,
                    const std::vector<std::string>& registered_clock_sats,
                    bool datum_changed,
                    const std::set<char>& changed_consts,
                    bool clk_model_warm,
                    bool emit_rate_warm,
                    double pl_now_s,
                    double dt,
                    std::set<std::string>& clock_refloat_sats,
                    std::set<std::string>& retro_flag_sats,
                    bool retro_feed) {
    // --- Alias preamble: copies of the former Application::runNetwork alias block
    //     and the reconstructed emitEvent / handover_affects lambdas, so the
    //     relocated SB_CLOCKUPD body below reads the original names. ------------
    const auto& nc = config_.getNetworkConfig();

    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   clk_mon_on    = ctx.knobs.clk_mon_on;       // per-sat carried-clock sanity monitor + refloat
    const double clk_mon_abs   = ctx.knobs.clk_mon_abs;  // |dclk - median| refloat bound (m)
    const double clk_mon_jump  = ctx.knobs.clk_mon_jump; // one-epoch clock jump refloat bound (m)
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this sigma; <=0: hard skip-stash refloat
    const double clk_mon_reinit_var = ctx.knobs.clk_mon_reinit_var;   // reinit variance folded next epoch for a flagged sat
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;    // anchor-free per-constellation clock gauge (sum dclk=0) instead of a pivot pin
    const bool   scoped_handover = ctx.knobs.scoped_handover;   // per-constellation scoping of the internal handover response (RTA-2)
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const double clk_model_k    = ctx.knobs.clk_model_k;
    const double clk_model_r = ctx.knobs.clk_model_r;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;  // gap bound for clock-model reseed, drift reset and C1 zero-order hold (R11)
    const bool   trace_flag_rates = ctx.knobs.trace_flag_rates;

    auto& prev_clk = ctx.carry.prev_clk;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& clock_rate_est = ctx.carry.clock_rate_est;

    auto& n_emitted = ctx.counters.n_emitted;
    auto& fr_clk_model = ctx.counters.fr_clk_model;
    auto& fr_clk_mon = ctx.counters.fr_clk_mon;

    auto& clk_mon_trace        = traces.clk_mon_trace;

    const auto& clk_pred = ws.clk_pred;

    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };

    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };

    // --- Two-state oscillator clock: measurement update + normalized-innovation fault test
    //     (network.clock_model) ------------------------------------------------------------
    // Update each satellite's clock model with this epoch's solved offset and test the
    // normalized innovation against the model's own tight prediction band.  A fault-free clock
    // tracks its prediction (|innov|/√S ~ 1); a clock locked onto a wrong but
    // self-consistent value departs from it by hundreds of σ.  That dynamics fault is
    // detectable here and not in the scalar random-walk carry.  The per-constellation
    // median innovation (the datum common mode) is removed first, so a whole-constellation
    // datum re-reference cancels and only per-satellite faults flag.  A flagged satellite is
    // added to clock_refloat_sats (its phase/code biases reinitialize in the stash) and its
    // clock model is loosened (large-q reinit) so it re-acquires toward the a-priori anchor
    // instead of freezing.  On a pivot handover the models are re-seeded (the datum moved).
    if (clk_model_warm) {
        if (datum_changed && !scoped_handover) {
            // Card PRM phase 3a: the entries the wipe removes.
            prmRespond(ctx.counters.prm_resp, ResponseSite::ClkOscWipe,
                       static_cast<long long>(clock_osc.size()));
            clock_osc.clear();   // datum moved: every carried offset is in the retired frame (t_last dropped with it)
            // Record the datum-change oscillator wipe as an event; nothing else reports it (T5, V2).
            emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt, std::nullopt,
                      "clear_osc", "datum re-reference");
        } else {
            // Under network.scoped_handover_response (RTA-2), wipe only the changed
            // constellations' oscillators, whose carried offsets are in the retired
            // frame; the other constellations' models survive and update normally
            // below.  A wiped satellite has no prediction this epoch (the scoped
            // prior skip in the prediction step), so it takes the seed branch and
            // re-seeds from this epoch's solve, which is already in the new datum.
            // That is one epoch earlier than the global wipe, which skips the whole
            // update block.
            if (datum_changed) {
                for (auto oit = clock_osc.begin(); oit != clock_osc.end();) {
                    if (datumChangeAffectsKey(oit->first, changed_consts, /*scoped=*/true)) {
                        oit = clock_osc.erase(oit);
                        prmRespond(ctx.counters.prm_resp, ResponseSite::ClkOscWipe);   // card PRM phase 3a
                    } else
                        ++oit;
                }
                std::string consts;
                for (char c : changed_consts) {
                    if (!consts.empty()) consts += ',';
                    consts += c;
                }
                emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt,
                          std::nullopt, "clear_osc",
                          fmt::format("datum re-reference (scoped: {})", consts));
            }
            std::map<std::string, double> z_of, innov_of, S_of, p_post_of;
            std::map<System, std::vector<double>> innov_by_const;
            const Eigen::MatrixXd& shcov = est.sharedCovariance();   // posterior formal cov
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                const double z = x_s(col);
                if (!std::isfinite(z) || sat.empty()) continue;
                z_of[sat] = z;
                p_post_of[sat] = (col < shcov.rows() && col < shcov.cols())
                                     ? shcov(col, col) : 0.0;   // posterior formal variance
                auto pit = clk_pred.find(sat);
                if (pit == clk_pred.end()) continue;      // freshly seeded or after a gap: no prediction
                innov_of[sat] = z - pit->second.x0;
                // The fault statistic uses the prior-minus-posterior variance
                // (p00_pred − p00_post), not p00_pred + clk_model_r (B3, data-incest fix).
                // The posterior x_s already folds the model prior, so its innovation against
                // the prediction is the shrunken residual, and its variance is the information
                // the data removed, p00_pred − p00_post.  Adding R would overstate S and deflate
                // the NIS in the weak-data regime the test is meant to police.
                // S is floored at the measurement-noise scale (σ_floor² = clk_model_r) rather
                // than 1e-12, so a weak-data epoch (p00_post → p00_pred ⇒ p00_pred − p00_post → 0,
                // and dm → 0) yields NIS ≈ 0 instead of a 0/0 blow-up that would false-flag the
                // lowest-confidence satellites.  The floor is a lower bound on S, not an R added
                // to a posterior-consistent innovation.
                S_of[sat]     = std::max(std::max(1e-12, clk_model_r),
                                         pit->second.p00 - p_post_of[sat]);
                innov_by_const[systemOfSat(sat)].push_back(innov_of[sat]);
            }
            std::map<System, double> med_innov;
            for (auto& [c, vv] : innov_by_const) {
                std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
                med_innov[c] = vv[vv.size() / 2];
            }
            int n_flag = 0;
            std::string detail;
            for (const std::string& sat : registered_clock_sats) {
                auto zit = z_of.find(sat);
                if (zit == z_of.end()) continue;
                const double z   = zit->second;
                ClockOsc&    osc = clock_osc[sat];
                auto pit = clk_pred.find(sat);
                if (pit == clk_pred.end() || !osc.init) {
                    // Seed (first warm epoch of this satellite, or its carried state was
                    // dropped as stale in the prediction step after an absence): start
                    // from the solved value, zero drift, loose covariance; no fault test
                    // until it has a prediction.  Stamp t_last so the next epoch's drift
                    // finite difference and prediction use the true per-sat interval.
                    // Card PRM phase 3a: a carried oscillator re-seeded past its maximum
                    // gap is the gap response (the prediction step's stale test, repeated
                    // on the same t_last, pl_now_s and dt; a first sighting is not
                    // counted, and neither is a prediction dropped by its finite screen).
                    if (osc.init) {
                        const double prm_gap_dt = (osc.t_last > 0.0) ? (pl_now_s - osc.t_last)
                                                                     : std::max(0.0, dt);
                        if (prm_gap_dt > clk_model_max_gap)
                            prmRespond(ctx.counters.prm_resp, ResponseSite::ClkOscGapReset);
                    }
                    osc.x0 = z; osc.x1 = 0.0;
                    osc.p00 = 1.0; osc.p01 = 0.0; osc.p11 = 1e-6;
                    osc.t_last = pl_now_s; osc.init = true;
                    continue;
                }
                const double S   = std::max(1e-12, S_of[sat]);
                const double dm  = innov_of[sat] - med_innov[systemOfSat(sat)];  // datum common-mode removed
                const double nis = dm / std::sqrt(S);
                const bool   ext = clock_refloat_sats.count(sat) > 0;  // within-epoch/RAIM (couple)
                const bool   flagged = (std::abs(nis) > clk_model_k) || ext;
                if (trace_flag_rates) { ++fr_clk_model.opp; if (std::abs(nis) > clk_model_k) ++fr_clk_model.flag; }  // own detections only, excluding ext (B8, T5)
                if (flagged && !ext) { ++n_flag; detail += (detail.empty() ? "" : ",") + sat;
                    emitEvent("T5_clock_model", "nis_fault", sat, "", nis, clk_model_k,
                              "refloat", fmt::format("innov={:.4f}", dm)); }
                if (flagged) clock_refloat_sats.insert(sat);
                // Set the carried state directly from the joint-solve posterior, without a
                // scalar KF measurement update (B3, data-incest fix): such an update would fold
                // the model prior a second time and make the oscillator covariance
                // optimistic.  The offset and its variance are the solve posterior; the drift
                // is finite-differenced from the offset sequence.  On a fault the variance is
                // reinitialized (loosened) so a frozen clock can re-acquire instead of locking
                // onto the bad value; the a-priori anchor pulls it back.
                const double p_post   = std::max(1e-12, p_post_of[sat]);
                const double x0_prev  = osc.x0;
                // Finite-difference the drift over this satellite's true elapsed time
                // (pl_now_s − osc.t_last), not the global epoch dt (B3b, P0-3).  When the last
                // clock-model value is several epochs old (intervening epochs skipped by a
                // gate/σ̂0 withhold or a solve failure, or the satellite absent), dividing the
                // multi-interval Δoffset by one dt would emit a drift ~N× too large as C1 next
                // epoch.  Past the gap bound the pair is not usable and the drift resets to 0;
                // within it the true interval gives the correct rate.
                const double sat_dt   = (osc.t_last > 0.0) ? (pl_now_s - osc.t_last) : dt;
                const bool contiguous = (osc.t_last > 0.0) && sat_dt > 0.0
                                        && sat_dt <= clk_model_max_gap;   // R11
                // Card PRM phase 3a: the drift reset past the maximum gap (not the reset of
                // a zero or unstamped interval).  The prediction step drops a stale
                // oscillator's prediction on the same test, so a stale return normally
                // re-seeds above; this counts the branch should it ever act.
                if (!contiguous && osc.t_last > 0.0 && sat_dt > clk_model_max_gap)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::ClkOscGapReset);
                // Card PRM phase 3a: the fault response re-initializes this oscillator.
                if (flagged)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::ClkOscReinit);
                osc.x1  = contiguous ? (z - x0_prev) / sat_dt : 0.0;
                osc.x0  = z;
                osc.p00 = flagged ? ((clk_mon_reinit_var > 0.0) ? clk_mon_reinit_var : (30.0 * 30.0))
                                  : p_post;                            // epoch formal variance
                osc.p11 = flagged ? (osc.p11 + 1e-2) : osc.p11;        // reopen drift on fault
                osc.p01 = 0.0;   // offset/drift cross term not tracked in the direct set
                osc.t_last = pl_now_s;   // this satellite's update time (P0-3)
                osc.init = true;
                // Trace (network.trace_slip_events): the clk_mon columns are remapped for the
                // model: clk_m=solved, median_m=predicted, dev_m=de-meaned innov,
                // prev_clk_m=drift, jump_m=raw innov, abs_max_m=band(√S), jump_max_m=k,
                // flag_abs=(nis>k).
                if (clk_mon_trace.is_open()
                    && (nc.trace_clock_full || flagged || std::abs(nis) > 0.3 * clk_model_k))
                    clk_mon_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << sat << ',' << fmt::format("{:.4f}", z) << ','
                        << fmt::format("{:.4f}", pit->second.x0) << ','
                        << fmt::format("{:.4f}", dm) << ','
                        << fmt::format("{:.6f}", osc.x1) << ','
                        << fmt::format("{:.4f}", innov_of[sat]) << ','
                        << fmt::format("{:.4f}", std::sqrt(S)) << ','
                        << fmt::format("{:.1f}", clk_model_k) << ','
                        << (std::abs(nis) > clk_model_k ? 1 : 0) << ",0,"
                        << (ext ? 1 : 0) << ',' << (flagged ? 1 : 0) << ','
                        << fmt::format("{:.4f}", nis) << ",model\n";   // nis and mode columns (L4)
            }
            if (n_flag > 0)
                log->warn("Network[clock-model]: TOW {:.1f} flagged {} satellite clock(s) [{}] — "
                          "normalized innovation > {:.1f}σ against the two-state prediction band "
                          "(dynamics fault); model loosened + carried state reinitialized.",
                          se.time.tow, n_flag, detail, clk_model_k);
            // osc.t_last (stamped above) records each satellite's last update time, so the
            // contiguity and gap decision is exact across skipped epochs (P0-3).
        }
    }

    // --- network.emit_clock_rate: decoupled C1 (clock-rate) estimator ---------------------
    // Finite-difference each satellite's posterior solved clock to derive the emitted C1,
    // without injecting a prior or driving a refloat, so the network solution is
    // byte-identical to the off state and only clock_c1_m_s (default 0) changes.  This is the
    // alternative to network.clock_model that the recalibration supports: the estimable is
    // white FM (docs/CLOCK_HADAMARD_CALIB), so the two-state carry prior improves stability
    // by only about 1× and risks bursts of NIS flags, while the deterministic rate emitted here is usable (a
    // low-latency consumer extrapolates c0 + c1·dt instead of a zero-order hold, which
    // removes the SSR saw-tooth).  Mirrors the clock-model drift computation's datum wipe and
    // gap/contiguity handling.  Runs only when clock_model is off (otherwise clock_osc
    // supplies C1 above); the knob loader clears emit_clock_rate_on when clock_model is on.
    if (emit_rate_warm) {
        // Datum re-reference: the carried offsets are in the retired frame.  Drop them so a
        // datum step is not finite-differenced into a spurious rate (as for clock_model).
        if (datum_changed && !scoped_handover) {
            clock_rate_est.clear();
        } else if (datum_changed) {
            for (auto rit = clock_rate_est.begin(); rit != clock_rate_est.end();) {
                if (datumChangeAffectsKey(rit->first, changed_consts, /*scoped=*/true))
                    rit = clock_rate_est.erase(rit);
                else
                    ++rit;
            }
        }
        for (const std::string& sat : registered_clock_sats) {
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            const double z = x_s(col);
            if (!std::isfinite(z) || sat.empty()) continue;
            ClockOsc& r = clock_rate_est[sat];   // default-constructs (init=false) on first sight
            // Finite-difference over this satellite's true elapsed time; past the R11 gap bound
            // the pair is not usable and the drift is 0 (C1 zero-order hold).  A first sighting
            // or a stale return seeds x1 = 0.
            const double sat_dt = (r.init && r.t_last > 0.0) ? (pl_now_s - r.t_last) : 0.0;
            r.x1 = (sat_dt > 0.0 && sat_dt <= clk_model_max_gap) ? (z - r.x0) / sat_dt : 0.0;
            r.x0 = z;
            r.t_last = pl_now_s;
            r.init = true;
        }
    }

    // --- Per-satellite carried-clock sanity monitor (network.clock_monitor) --------------
    // The σ̂0 RAIM flags contamination that moves the shared residual level (many stations
    // spike at once).  It does not see a single satellite whose carried clock has locked
    // onto a physically impossible value: the clock absorbs the error self-consistently,
    // σ̂0 stays ≈1, and the stiff sat_clock_q carry then holds the wrong value for the rest
    // of the run.  This monitor detects that mode.  On the final (post-adaptation) solution
    // it flags any satellite whose de-meaned clock exceeds clk_mon_abs or whose one-epoch
    // jump exceeds clk_mon_jump, and refloats its carried shared state (the stash skips it),
    // so the next epoch injects no stale prior and the clock re-estimates freely toward its
    // data and a-priori-anchor value.  De-meaning by the median removes the per-epoch datum
    // gauge (a few metres, not error); the jump test runs only across adjacent stable-datum
    // epochs.  clock_refloat_sats may already hold within-epoch / post-fit RAIM satellites
    // (only under clock_monitor_couple_raim); those are refloated here too (ext).
    //
    // Gated on the emitted-epoch warmup, not carry_warm: the D2 shared clock carry runs
    // independently of phase leveling, so the monitor does too.  It only needs to be past the
    // cold-start systematic (the same pl_warmup horizon the ambiguity carry uses).  With
    // network.clock_model on, the two-state normalized-innovation test above replaces it.
    if (clk_mon_on && !clk_model_on && static_cast<int>(n_emitted) >= pl_warmup) {
        std::map<std::string, double> clk_of;
        std::map<System, std::vector<double>> clk_by_const;  // per-constellation, for the datum median
        for (const std::string& sat : registered_clock_sats) {
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            const double v = x_s(col);
            if (!std::isfinite(v) || sat.empty()) continue;
            clk_of[sat] = v;
            clk_by_const[systemOfSat(sat)].push_back(v);
        }
        // De-mean per constellation.  Each constellation has its own clock datum (a distinct
        // pivot satellite), so a whole-constellation offset, such as a Galileo-wide
        // re-reference when its pivot hands over, is a gauge artifact and not per-satellite
        // error.  A single global median would subtract the GPS datum from the Galileo clocks
        // and flag the entire constellation; the per-constellation median cancels that and
        // leaves only the per-satellite outliers within each system.
        std::map<System, double> med_by_const;
        for (auto& [c, vv] : clk_by_const) {
            std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
            med_by_const[c] = vv[vv.size() / 2];
        }
        int n_new = 0;
        std::string detail;
        for (const auto& [sat, v] : clk_of) {
            const double med      = med_by_const[systemOfSat(sat)];
            const double dev      = v - med;
            const bool   flag_abs = std::abs(dev) > clk_mon_abs;
            // Jump-test reference frame.  Pin mode uses the raw clock v: the pivot pin
            // anchors the raw per-constellation frame across epochs, so v−prev_v is a
            // per-sat step (byte-identical to the legacy path).  Sum-to-zero mode uses the
            // per-constellation de-meaned clock dev, because the raw common mode floats
            // each epoch: Σδtˢ=0 is re-imposed on the current satellite set, so a
            // membership change redistributes ~Δ/N onto every survivor's raw clock, which
            // is a datum artifact and not a per-sat jump.  De-meaning by the constellation
            // median removes that float, as `innov − median` does in the two-state
            // clock-model NIS path.  Broadcast-anchored mode uses the raw v like pin mode:
            // every satellite is anchored independently to its own broadcast a-priori, so
            // membership changes cause no redistribution and v−prev_v is a per-sat step.
            const double jref     = clock_sum_to_zero ? dev : v;
            bool   have_jump  = false;
            double jump       = 0.0;
            bool   flag_jump  = false;
            const auto pit = prev_clk.find(sat);
            // Jump only across adjacent epochs of a stable datum: a handover shifts every
            // clock of the re-referenced constellation (common mode, not a per-sat fault)
            // and a long gap makes the step meaningless; both fall back to the absolute
            // bound.  Under network.scoped_handover_response (RTA-2) the datum is stable
            // for constellations whose pivot did not change, so their jump test keeps
            // running through another constellation's handover.  With it off,
            // handover_affects == datum_changed and the legacy all-constellation
            // suppression is byte-identical.
            if (pit != prev_clk.end() && !handover_affects(sat) && dt > 0.0
                && (pl_now_s - pit->second.second) <= 1.5 * dt) {
                have_jump = true;
                jump      = jref - pit->second.first;   // datum-invariant under sum-to-zero (jref=dev), raw under pin (jref=v)
                flag_jump = std::abs(jump) > clk_mon_jump;
            }
            const bool ext       = clock_refloat_sats.count(sat) > 0;  // within-epoch / post-fit RAIM
            const bool refloated = flag_abs || flag_jump || ext;
            if (trace_flag_rates) { ++fr_clk_mon.opp; if (flag_abs || flag_jump) ++fr_clk_mon.flag; }  // own bounds only, excluding ext (B8, T6)
            if (refloated && !ext) { ++n_new; detail += (detail.empty() ? "" : ",") + sat;
                emitEvent("T6_clock_monitor", "clk_monitor_refloat", sat, "", dev, clk_mon_abs,
                          "refloat",
                          have_jump ? fmt::format("jump={:.4f};jmax={:.2f}", jump, clk_mon_jump)
                                    : std::string("abs")); }
            if (refloated) clock_refloat_sats.insert(sat);

            if (clk_mon_trace.is_open()
                && (nc.trace_clock_full || refloated || std::abs(dev) > 0.5 * clk_mon_abs
                    || (have_jump && std::abs(jump) > 0.5 * clk_mon_jump))) {
                clk_mon_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << sat << ',' << fmt::format("{:.4f}", v) << ','
                    << fmt::format("{:.4f}", med) << ',' << fmt::format("{:.4f}", dev) << ',';
                if (have_jump)
                    clk_mon_trace << fmt::format("{:.4f}", pit->second.first) << ','
                                  << fmt::format("{:.4f}", jump) << ',';
                else
                    clk_mon_trace << ",,";
                clk_mon_trace
                    << fmt::format("{:.2f}", clk_mon_abs) << ','
                    << fmt::format("{:.2f}", clk_mon_jump) << ','
                    << (flag_abs ? 1 : 0) << ',' << (flag_jump ? 1 : 0) << ','
                    << (ext ? 1 : 0) << ',' << (refloated ? 1 : 0) << ",,monitor\n";  // empty nis column, then mode (L4)
            }

            // Re-baseline the jump test: a refloated sat's clock is about to be
            // reinitialized, so drop its history (next epoch's recovery must not read as a
            // fresh jump); otherwise record this epoch's jump reference (jref) + time as the
            // next adjacent baseline (raw v in pin mode; de-meaned dev in sum-to-zero mode).
            if (refloated) prev_clk.erase(sat);
            else           prev_clk[sat] = {jref, pl_now_s};
        }
        if (n_new > 0)
            log->warn("Network[clock-monitor]: TOW {:.1f} reinitialized {} satellite clock(s) [{}] "
                      "exceeding |Δδtˢ−median| > {:.1f} m or |jump| > {:.1f} m/epoch — carried "
                      "shared state {} to break the single-satellite lock-in.",
                      se.time.tow, n_new, detail, clk_mon_abs, clk_mon_jump,
                      (clk_mon_reinit_sig > 0.0)
                          ? fmt::format("loosened to σ={:.0f} m (soft large-q reinit)", clk_mon_reinit_sig)
                          : std::string("withheld from the stash (hard refloat)"));
    }

    networkRetroUraBookkeeping(ctx, clock_refloat_sats, retro_flag_sats, retro_feed);

    // === SPEC_day129_frame_guards Phase 2.1 ===================================
    // Network-wide common-mode clock-excursion detector, which reinitializes the
    // code-bias carry.
    //
    // The statistic is the median, not the max.  The solve can slide along the cheap
    // global mode {all satellite clocks −δ, all receiver clocks −δ}, along which phase
    // and code residuals are invariant except at the two hard pins.  The pinned
    // satellite's clock cannot move, so the pressure that every other satellite
    // takes up in its soft clock (q = 0.3 m²/epoch, recovered in 2–4 epochs) lands
    // instead on the pivot's near-static code biases (q = 3e-9 m²/epoch, gain
    // K ≈ 2.2e-3/epoch, bleed τ ≈ 3.8 h).  The median of |Δclock| measures the
    // one-sided (common) part of an excursion, the part that projects onto the pins,
    // while a max-based statistic fires on any single-satellite outlier, which the
    // clock monitor above already handles.  On DOY-129 the two statistics separate
    // the harmless from the damaging event (see the calibration table on
    // NetworkConfig::cb_excursion_median_m).
    //
    // Reset, not repair: a tripped epoch reinitializes the pivot's CB carry (the
    // existing inflate_q path) so the next epoch re-estimates it from data with a
    // weak anchor, instead of carrying a stale near-static value through the
    // excursion.  No measurement is dropped.
    //
    // Off (default): the whole block is skipped; no baseline map is maintained, no
    // set is written and no bump is queued, so the output is byte-identical.
    if (nc.cb_excursion_reinit && nc.cb_excursion_median_m > 0.0) {
        auto& prev = ctx.carry.cb_exc_prev_clk;
        auto& mark = ctx.carry.cb_excursion_sats;
        mark.clear();   // per-epoch handoff to this epoch's stash

        std::map<std::string, double> now;
        std::vector<double> deltas;
        std::vector<std::string> jumpers;
        for (const std::string& sat : registered_clock_sats) {
            const Eigen::Index col = est.clockColumn(sat);
            if (col < 0 || col >= x_s.size()) continue;
            const double v = x_s(col);
            if (!std::isfinite(v)) continue;
            now[sat] = v;
            const auto pit = prev.find(sat);
            if (pit == prev.end()) continue;
            const double d = std::abs(v - pit->second);
            deltas.push_back(d);
            if (d > nc.cb_excursion_median_m) jumpers.push_back(sat);
        }

        // A clock-pivot handover re-references every clock of the affected
        // constellation by a common constant, which is a datum artifact and not an
        // excursion.  The test is suppressed on any datum change, network-wide (the
        // clock monitor's jump test suppresses for the same reason, but per
        // constellation under network.scoped_handover_response), and it also needs
        // at least 8 satellites with a baseline.  The baseline is still updated, so
        // the epoch after a handover compares like with like.
        const bool testable = !datum_changed && deltas.size() >= 8;
        if (testable) {
            std::vector<double> s = deltas;
            std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2),
                             s.end());
            const double med = s[s.size() / 2];
            if (med > ctx.carry.cb_exc_max_median) {
                ctx.carry.cb_exc_max_median     = med;
                ctx.carry.cb_exc_max_median_tow = se.time.tow;
            }
            if (med > nc.cb_excursion_median_m) {
                ++ctx.carry.cb_exc_trips;
                const double mx = *std::max_element(deltas.begin(), deltas.end());
                // Which satellites' CB carry is reinitialized (D1).  Pivots only by
                // default, since the pivot is the one satellite whose clock↔code split
                // is both data-exposed and stiffly carried; all satellites under D1b
                // (nc.cb_excursion_reinit_all_sats).
                if (nc.cb_excursion_reinit_all_sats) {
                    for (const auto& kv : now) mark.insert(kv.first);
                } else {
                    for (const auto& kv : ctx.carry.prev_pivot)
                        if (!kv.second.empty() && now.count(kv.second))
                            mark.insert(kv.second);
                }
                // Count how often a per-satellite rule would also have caught a
                // non-pivot satellite (D1 evidence counter).  It provides the data for choosing
                // between pivot-only and all-satellite reinitialization.
                std::size_t n_nonpivot_jumpers = 0;
                for (const std::string& s2 : jumpers)
                    if (!mark.count(s2)) { ++n_nonpivot_jumpers; ++ctx.carry.cb_exc_nonpivot_would; }

                ctx.carry.cb_exc_reinits += static_cast<long long>(mark.size());
                // A value step has to coincide with a signalled discontinuity (Card-2
                // rule).  The reinit lands in the next epoch's fold, so the bump is
                // queued for the next emit (consumed in NetworkSolPrep).
                for (const std::string& s2 : mark)
                    ctx.carry.cb_excursion_pending_bump.insert(s2);

                std::string marked;
                for (const std::string& s2 : mark) marked += (marked.empty() ? "" : ";") + s2;
                log->warn("Network[CB-excursion]: TOW {:.1f} network-wide common-mode clock "
                          "excursion — median |Δδtˢ| {:.2f} m > {:.2f} m (max {:.2f} m, "
                          "max/median {:.2f} — {}).  Reinitializing the code-bias carry of "
                          "[{}] (σ={:.0f} m next epoch) and queueing a code-bias "
                          "discontinuity bump at the next emit.  {} non-pivot satellite(s) "
                          "also exceeded the bound this epoch.",
                          se.time.tow, med, nc.cb_excursion_median_m, mx,
                          med > 0.0 ? mx / med : 0.0,
                          (med > 0.0 && mx / med < 2.0) ? "ONE-SIDED, reaches the pins"
                                                        : "two-sided-ish",
                          marked, clk_mon_reinit_sig, n_nonpivot_jumpers);
                emitEvent("CB_EXC", "cb_excursion_reinit", marked, "", med,
                          nc.cb_excursion_median_m, "reinit",
                          fmt::format("max={:.2f};ratio={:.2f};n_jump={}", mx,
                                      med > 0.0 ? mx / med : 0.0, jumpers.size()));
            }
        }
        prev = std::move(now);
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the CB-excursion guard (Phase 2.1).
int networkCbExcursionRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // CB-excursion guard run-end telemetry (SPEC_day129_frame_guards Phase 2.1).
    // "0 trips / 0 reinits" means the guard is inert however clean the run looks
    // (invariant #4); this line is the ON-path evidence, and the validation gate reads it.
    if (!config_.getNetworkConfig().cb_excursion_reinit) return 0;
    log->info("Network (Phase 2.1 CB-excursion guard): {} trip(s), {} code-bias "
              "carry reinit(s), {} discontinuity bump(s) landed; largest median "
              "|Δδtˢ| observed {:.2f} m at TOW {:.1f} (threshold {:.2f} m).  D1 "
              "evidence: {} (satellite,epoch) NON-pivot case(s) a per-satellite "
              "rule would have reinitialized — scope is {}.",
              ctx.carry.cb_exc_trips, ctx.carry.cb_exc_reinits,
              ctx.carry.cb_exc_bumps, ctx.carry.cb_exc_max_median,
              ctx.carry.cb_exc_max_median_tow,
              config_.getNetworkConfig().cb_excursion_median_m,
              ctx.carry.cb_exc_nonpivot_would,
              config_.getNetworkConfig().cb_excursion_reinit_all_sats
                  ? "ALL satellites (D1b)" : "clock-PIVOT only (D1)");
    if (ctx.carry.cb_exc_trips == 0)
        log->warn("Network (Phase 2.1): the CB-excursion guard is ARMED but never "
                  "tripped over this run — it is INERT here.  Either the window "
                  "carries no common-mode excursion, or the threshold ({:.2f} m) is "
                  "above anything that occurred (largest observed median {:.2f} m).  "
                  "Do NOT read this run as evidence the guard works.",
                  config_.getNetworkConfig().cb_excursion_median_m,
                  ctx.carry.cb_exc_max_median);
    if (!ctx.carry.cb_excursion_pending_bump.empty())
        log->warn("Network (Phase 2.1): {} queued code-bias discontinuity bump(s) "
                  "never landed (their satellites were absent from every subsequent "
                  "emission) — the reinit stepped a value the product did not signal.",
                  ctx.carry.cb_excursion_pending_bump.size());
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
