/**
 * @file NetworkClockQc.cpp
 * @brief Implementation of @ref networkClockQc — the post-solve satellite-clock QC
 *        block relocated VERBATIM (dedent-only, zero substitution) out of
 *        Application::runNetwork at decomposition INC-10 (2026-07-22).
 *
 * The body below is a byte-copy of the former SB_CLOCKUPD block
 * (ApplicationNetwork.cpp lines 7099-7421), dedented 4 spaces (the block sat at
 * loop-body indent, no wrapping if).  The alias preamble reconstructs the run-TU
 * alias block (nc, clk_pred, the knob/carry/counter/trace aliases) plus the
 * emitEvent + handover_affects lambdas, so the relocated code reads the original
 * names.  See NetworkClockQc.hpp for the ordering contract.
 */

#include "filter/NetworkClockQc.hpp"

#include "app/NetworkRunContext.hpp"          // ctx.* + networkRetroUraBookkeeping
#include "config/GnssConfig.hpp"              // config_.getNetworkConfig() (nc.trace_clock_full)
#include "filter/NetworkPivotHandover.hpp"    // datumChangeAffectsKey
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
    // --- Alias preamble: byte-copies of the former Application::runNetwork alias
    //     block + the reconstructed emitEvent / handover_affects lambdas, so the
    //     relocated SB_CLOCKUPD body below reads the original names. ------------
    const auto& nc = config_.getNetworkConfig();

    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   clk_mon_on    = ctx.knobs.clk_mon_on;       // per-sat carried-clock sanity monitor + refloat
    const double clk_mon_abs   = ctx.knobs.clk_mon_abs;  // |dclk - median| refloat bound (m)
    const double clk_mon_jump  = ctx.knobs.clk_mon_jump; // one-epoch clock jump refloat bound (m)
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this sigma; <=0: hard skip-stash refloat
    const double clk_mon_reinit_var = ctx.knobs.clk_mon_reinit_var;   // reinit variance folded next epoch for a flagged sat
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;    // anchor-free per-constellation clock gauge (sum dclk=0) instead of a pivot pin
    const bool   scoped_handover = ctx.knobs.scoped_handover;   // RTA-2: per-constellation scoping of the INTERNAL handover response
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const double clk_model_k    = ctx.knobs.clk_model_k;
    const double clk_model_r = ctx.knobs.clk_model_r;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;  // R11: clock-model carry reseed / drift-reset / C1-ZOH bound
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
    // normalized innovation against the model's own tight prediction band.  A healthy clock
    // tracks its prediction (|innov|/√S ~ 1); a self-consistent lock-in jumps hundreds of σ —
    // the detectable dynamics fault the scalar random-walk carry hides.  The per-constellation
    // MEDIAN innovation (the datum common-mode) is removed first, so a whole-constellation
    // datum re-reference is gauged out and only per-satellite faults flag.  A flagged sat is
    // added to clock_refloat_sats (its phase/code bias reinitialize in the stash) and its
    // clock model is loosened (large-q reinit) so it re-acquires toward the a-priori anchor
    // instead of freezing.  On a pivot handover the models are re-seeded (the datum moved).
    if (clk_model_warm) {
        if (datum_changed && !scoped_handover) {
            clock_osc.clear();   // datum moved: every carried offset is in the retired frame (t_last dropped with it)
            // T5 (V2 blind spot): the datum-change oscillator wipe is otherwise SILENT.
            emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt, std::nullopt,
                      "clear_osc", "datum re-reference");
        } else {
            // RTA-2 (network.scoped_handover_response): wipe ONLY the changed
            // constellation(s)' oscillators — their carried offsets are in the
            // retired frame; unaffected constellations' models survive and update
            // normally below.  A wiped satellite carries no prediction this epoch
            // (the scoped prior skip above), so it falls into the seed branch and
            // re-seeds from THIS epoch's already-new-datum solve — one epoch
            // earlier than the global wipe, which skips the whole update block.
            if (datum_changed) {
                for (auto oit = clock_osc.begin(); oit != clock_osc.end();) {
                    if (datumChangeAffectsKey(oit->first, changed_consts, /*scoped=*/true))
                        oit = clock_osc.erase(oit);
                    else
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
            std::map<char, std::vector<double>> innov_by_const;
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
                if (pit == clk_pred.end()) continue;      // freshly seeded / gap — no prediction
                innov_of[sat] = z - pit->second.x0;
                // B3 (data-incest fix): the fault statistic uses the info-consistent
                // prior-minus-posterior variance (p00_pred − p00_post), NOT the old incest
                // R-add (pit->p00 + clk_model_r).  The posterior x_s already FOLDS the model
                // prior, so its innovation vs the prediction is the shrunken residual; its
                // correct variance is the information the data removed, p00_pred − p00_post.
                // Using +R deflated S in exactly the weak-data regime the NIS is meant to police.
                // Floor S with the measurement-noise scale (σ_floor² = clk_model_r), NOT 1e-12, so
                // a weak-data epoch (p00_post → p00_pred ⇒ p00_pred − p00_post → 0, and dm → 0)
                // yields NIS ≈ 0 rather than a 0/0 blow-up that would false-flag the lowest-
                // confidence satellites.  This is a LOWER BOUND, not the incest of adding R to a
                // posterior-consistent innovation.
                S_of[sat]     = std::max(std::max(1e-12, clk_model_r),
                                         pit->second.p00 - p_post_of[sat]);
                innov_by_const[sat[0]].push_back(innov_of[sat]);
            }
            std::map<char, double> med_innov;
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
                    // Seed (first warm epoch of this satellite, OR its carried state was
                    // dropped as stale in the prediction above — a vanish-return): start
                    // from the solved value, zero drift, loose covariance; no fault test
                    // until it has a prediction.  Stamp t_last so the NEXT epoch's drift
                    // finite-difference and prediction use the true per-sat interval.
                    osc.x0 = z; osc.x1 = 0.0;
                    osc.p00 = 1.0; osc.p01 = 0.0; osc.p11 = 1e-6;
                    osc.t_last = pl_now_s; osc.init = true;
                    continue;
                }
                const double S   = std::max(1e-12, S_of[sat]);
                const double dm  = innov_of[sat] - med_innov[sat[0]];  // datum common-mode removed
                const double nis = dm / std::sqrt(S);
                const bool   ext = clock_refloat_sats.count(sat) > 0;  // within-epoch/RAIM (couple)
                const bool   flagged = (std::abs(nis) > clk_model_k) || ext;
                if (trace_flag_rates) { ++fr_clk_model.opp; if (std::abs(nis) > clk_model_k) ++fr_clk_model.flag; }  // B8/T5 own detection (excl. ext)
                if (flagged && !ext) { ++n_flag; detail += (detail.empty() ? "" : ",") + sat;
                    emitEvent("T5_clock_model", "nis_fault", sat, "", nis, clk_model_k,
                              "refloat", fmt::format("innov={:.4f}", dm)); }
                if (flagged) clock_refloat_sats.insert(sat);
                // B3 (data-incest fix): set the carried state DIRECTLY from the joint-solve
                // posterior — NO scalar KF measurement update (that folded the model prior a
                // second time, making the osc covariance self-confirmingly optimistic).  The
                // offset + its variance ARE the solve posterior; the drift is finite-differenced
                // from the offset sequence.  On a fault, reinit the variance (loosen) so a frozen
                // clock can re-acquire rather than lock onto the bad value; the a-priori anchor
                // pulls it back to truth.
                const double p_post   = std::max(1e-12, p_post_of[sat]);
                const double x0_prev  = osc.x0;
                // B3b/P0-3: finite-difference the drift over THIS satellite's true elapsed
                // time (pl_now_s − osc.t_last), NOT the global epoch dt.  A satellite whose
                // last clock-model value came several epochs ago — because intervening epochs
                // were SKIPPED (gate/σ̂0 withhold, solve failure) or the sat was absent —
                // would otherwise divide a multi-interval Δoffset by one dt and emit a drift
                // ~N× too large as C1 next epoch.  Past a gap bound the pair is not usable
                // (reset drift to 0); within it the true interval gives the correct rate.
                const double sat_dt   = (osc.t_last > 0.0) ? (pl_now_s - osc.t_last) : dt;
                const bool contiguous = (osc.t_last > 0.0) && sat_dt > 0.0
                                        && sat_dt <= clk_model_max_gap;   // R11
                osc.x1  = contiguous ? (z - x0_prev) / sat_dt : 0.0;
                osc.x0  = z;
                osc.p00 = flagged ? ((clk_mon_reinit_var > 0.0) ? clk_mon_reinit_var : (30.0 * 30.0))
                                  : p_post;                            // epoch formal variance
                osc.p11 = flagged ? (osc.p11 + 1e-2) : osc.p11;        // reopen drift on fault
                osc.p01 = 0.0;   // offset/drift cross term not tracked in the direct set
                osc.t_last = pl_now_s;   // P0-3: stamp this sat's update time
                osc.init = true;
                // Trace (network.trace_slip_events): the clk_mon columns are remapped for the
                // model — clk_m=solved, median_m=predicted, dev_m=de-meaned innov, prev_clk_m=
                // drift, jump_m=raw innov, abs_max_m=band(√S), jump_max_m=k, flag_abs=(nis>k).
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
                        << fmt::format("{:.4f}", nis) << ",model\n";   // L4: nis + mode
            }
            if (n_flag > 0)
                log->warn("Network[clock-model]: TOW {:.1f} flagged {} satellite clock(s) [{}] — "
                          "normalized innovation > {:.1f}σ against the two-state prediction band "
                          "(dynamics fault); model loosened + carried state reinitialized.",
                          se.time.tow, n_flag, detail, clk_model_k);
            // P0-3: per-satellite osc.t_last (stamped above) now records each sat's last
            // update time directly, so the contiguity / gap decision is exact even across
            // skipped epochs — the old whole-set prev_osc_sats marker is retired.
        }
    }

    // --- network.emit_clock_rate: decoupled C1 (clock-rate) estimator ---------------------
    // Finite-difference each satellite's POSTERIOR solved clock to derive the emitted C1,
    // WITHOUT injecting any prior or driving any refloat — so the network solution is
    // byte-identical to the off state; only clock_c1_m_s (default 0) changes.  This is the
    // recalibration-supported alternative to network.clock_model: the estimable is white-FM
    // (docs/CLOCK_HADAMARD_CALIB), so the two-state CARRY prior buys ~1× yet risks a NIS
    // storm, but the deterministic RATE emitted here is the exploitable stability (a
    // low-latency consumer extrapolates c0 + c1·dt instead of ZOH — the SSR saw-tooth fix).
    // Mirrors the clock-model drift computation's datum-wipe + gap/contiguity handling; runs
    // only when clock_model is OFF (else clock_osc already supplies C1 above).
    if (emit_rate_warm) {
        // Datum re-reference: the carried offsets are in the retired frame — drop them so a
        // datum STEP is not finite-differenced into a spurious rate (mirror clock_model).
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
            // Finite-difference over THIS sat's true elapsed time; past the R11 gap bound the
            // pair is not usable → drift 0 (C1 ZOH).  First sighting / stale-return seeds x1=0.
            const double sat_dt = (r.init && r.t_last > 0.0) ? (pl_now_s - r.t_last) : 0.0;
            r.x1 = (sat_dt > 0.0 && sat_dt <= clk_model_max_gap) ? (z - r.x0) / sat_dt : 0.0;
            r.x0 = z;
            r.t_last = pl_now_s;
            r.init = true;
        }
    }

    // --- Per-satellite carried-clock sanity monitor (network.clock_monitor) --------------
    // The σ̂0 RAIM above flags contamination that MOVES the shared residual level (many
    // innocent stations spike at once).  A lone satellite whose carried clock has locked
    // into a physically impossible value is INVISIBLE to it — the clock absorbs the error
    // self-consistently, σ̂0 stays ≈1, and the stiff sat_clock_q carry then FREEZES the
    // wrong value for the rest of the run (the multi-hour single-satellite lock-in).  This
    // is the missing detector for that mode: on the FINAL (post-adaptation) solution, flag
    // any satellite whose DE-MEANED clock exceeds clk_mon_abs or whose one-epoch jump
    // exceeds clk_mon_jump, and refloat its carried shared state (the stash below skips it)
    // so the next epoch injects no stale prior and the clock re-estimates freely toward its
    // data / a-priori-anchor value.  De-meaning by the network median removes the per-epoch
    // datum gauge (a few metres, not error); the jump test runs only across adjacent
    // stable-datum epochs.  clock_refloat_sats may already hold within-epoch / post-fit RAIM
    // sats (only under clock_monitor_couple_raim); those are refloated here too (ext).
    //
    // Gated on the emitted-epoch warmup (NOT carry_warm): the D2 shared clock carry runs
    // independently of phase leveling, so the monitor must too; it only needs to be past the
    // cold-start systematic (the same pl_warmup horizon the ambiguity carry uses).  Superseded
    // by the two-state normalized-innovation test above when network.clock_model is on.
    if (clk_mon_on && !clk_model_on && static_cast<int>(n_emitted) >= pl_warmup) {
        std::map<std::string, double> clk_of;
        std::map<char, std::vector<double>> clk_by_const;  // per-constellation, for the datum median
        for (const std::string& sat : registered_clock_sats) {
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            const double v = x_s(col);
            if (!std::isfinite(v) || sat.empty()) continue;
            clk_of[sat] = v;
            clk_by_const[sat[0]].push_back(v);
        }
        // De-mean PER CONSTELLATION.  Each constellation has its OWN clock datum (a distinct
        // pivot satellite), so a whole-constellation offset — e.g. a Galileo-wide re-reference
        // when its pivot hands over — is a GAUGE artifact, not per-satellite error.  A single
        // global median would subtract the GPS datum from the Galileo clocks and flag the
        // entire constellation; the per-constellation median cancels that and leaves only the
        // genuine per-satellite outliers within each system.
        std::map<char, double> med_by_const;
        for (auto& [c, vv] : clk_by_const) {
            std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
            med_by_const[c] = vv[vv.size() / 2];
        }
        int n_new = 0;
        std::string detail;
        for (const auto& [sat, v] : clk_of) {
            const double med      = med_by_const[sat[0]];
            const double dev      = v - med;
            const bool   flag_abs = std::abs(dev) > clk_mon_abs;
            // Jump-test reference frame.  PIN mode: the raw clock v (the pivot pin
            // anchors the raw per-constellation frame across epochs, so v−prev_v is a
            // genuine per-sat step — byte-identical to the legacy path).  SUM-TO-ZERO
            // mode: the per-constellation DE-MEANED clock dev, because the raw
            // common-mode floats each epoch (Σδtˢ=0 is re-imposed on the current
            // satellite set, so a membership change redistributes ~Δ/N onto EVERY
            // survivor's raw clock — a datum artifact, not a per-sat jump).  De-meaning
            // by the constellation median removes that float, mirroring the two-state
            // clock-model NIS path's `innov − median`.  BROADCAST-ANCHORED mode keeps
            // the raw v like PIN: every satellite is anchored independently to its own
            // broadcast a-priori, so membership changes cause no redistribution and
            // v−prev_v is a genuine per-sat step.
            const double jref     = clock_sum_to_zero ? dev : v;
            bool   have_jump  = false;
            double jump       = 0.0;
            bool   flag_jump  = false;
            const auto pit = prev_clk.find(sat);
            // Jump only across ADJACENT epochs of a stable datum: a handover shifts every
            // clock of the re-referenced constellation (common-mode, not a per-sat fault)
            // and a long gap makes the step meaningless — both fall back to the absolute
            // bound.  RTA-2: under network.scoped_handover_response the datum is stable
            // for constellations whose pivot did NOT change, so their jump test keeps
            // running through a sibling's handover; OFF, handover_affects == datum_changed
            // and the legacy all-constellation suppression is byte-identical.
            if (pit != prev_clk.end() && !handover_affects(sat) && dt > 0.0
                && (pl_now_s - pit->second.second) <= 1.5 * dt) {
                have_jump = true;
                jump      = jref - pit->second.first;   // datum-invariant under sum-to-zero (jref=dev), raw under pin (jref=v)
                flag_jump = std::abs(jump) > clk_mon_jump;
            }
            const bool ext       = clock_refloat_sats.count(sat) > 0;  // within-epoch / post-fit RAIM
            const bool refloated = flag_abs || flag_jump || ext;
            if (trace_flag_rates) { ++fr_clk_mon.opp; if (flag_abs || flag_jump) ++fr_clk_mon.flag; }  // B8/T6 own bounds (excl. ext)
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
                    << (ext ? 1 : 0) << ',' << (refloated ? 1 : 0) << ",,monitor\n";  // L4: empty nis + mode
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
}

} // namespace GnssProcessor
