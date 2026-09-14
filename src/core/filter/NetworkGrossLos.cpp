/**
 * @file NetworkGrossLos.cpp
 * @brief Implementation of card GL — the gross line-of-sight screen
 *        (docs/SPEC_event_harness.md §24.18).  See NetworkGrossLos.hpp for why
 *        this mechanism exists and what its contract is.
 *
 * The row walk below is deliberately the SAME walk and the SAME family
 * classification as @c networkPostfitSigma0 (`filter/NetworkFrameDiag.cpp`),
 * because that pass is what MEASURED the residuals this screen acts on: if the
 * two ever disagreed about which rows are code, the grade (`network_pf_rows.csv`)
 * and the response would be describing different populations.  What differs is
 * only that this one is not gated on a trace being open, and that it ACTS.
 */

#include "filter/NetworkGrossLos.hpp"

#include "app/NetworkRunContext.hpp"      // ctx.knobs / ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"          // config_.getNetworkConfig()
#include "filter/StateManager.hpp"        // markAmbiguityPendingReseed
#include "logging/NetworkRunTraces.hpp"   // emitEvent

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// The PURE rules (unit-tested in tests/test_network_gross_los.cpp)
// ---------------------------------------------------------------------------

GrossLosVerdict networkGrossLosAssess(const std::vector<GrossLosSample>& rows,
                                      double k, int min_rows, double clear_sigma) {
    GrossLosVerdict d;
    d.n_rows = static_cast<int>(rows.size());
    if (rows.empty()) return d;   // absence is not evidence of health — under_clear stays false
    std::set<std::string> sigs;
    bool all_under_clear = true;
    for (const auto& s : rows) {
        if (!std::isfinite(s.r)) continue;
        if (s.r > d.r_max) d.r_max = s.r;
        const double av = std::abs(s.v_m);
        if (av > d.v_abs_max) d.v_abs_max = av;
        if (s.r > k) {
            ++d.n_over;
            if (!s.signal.empty()) sigs.insert(s.signal);
        }
        if (s.r > clear_sigma) all_under_clear = false;
    }
    d.over = d.n_over >= std::max(1, min_rows);
    // The release evidence is NOT `!over`: the band between clear_sigma and k is
    // neither, and a line of sight sitting there keeps whatever disposition it
    // has.  That gap is what makes the hysteresis real rather than nominal.
    d.under_clear = all_under_clear;
    for (const auto& s : sigs) d.signals += (d.signals.empty() ? "" : ";") + s;
    return d;
}

GrossLosEdge networkGrossLosStep(GrossLosState& s, const GrossLosVerdict& v,
                                 int declare_epochs, int clear_epochs, double tow,
                                 bool allow_declare) {
    const int need_dec = std::max(1, declare_epochs);
    const int need_clr = std::max(1, clear_epochs);
    if (!s.gross) {
        s.clear_streak = 0;
        if (!v.over) { s.over_streak = 0; return GrossLosEdge::None; }
        ++s.over_streak;
        if (s.over_streak < need_dec) return GrossLosEdge::None;
        if (!allow_declare) {
            // A guard holds the declaration.  HOLD the streak at the requirement:
            // discarding real evidence would make the guard a memory hole, and
            // letting it grow would turn the gate's opening into a burst of
            // simultaneous declarations.  Held, the line of sight declares on the
            // first qualifying epoch after the gate opens and on no earlier one.
            s.over_streak = need_dec;
            return GrossLosEdge::Suppressed;
        }
        s.gross        = true;
        s.over_streak  = 0;           // the evidence is spent; a re-declaration must re-persist
        s.clear_streak = 0;
        s.declared_tow = tow;
        ++s.n_declared;
        return GrossLosEdge::Declare;
    }
    // Declared.  Only the release bar matters; over-bar evidence simply resets
    // the clearing progress (it is the same line of sight still being wrong).
    if (!v.under_clear) { s.clear_streak = 0; return GrossLosEdge::None; }
    ++s.clear_streak;
    if (s.clear_streak < need_clr) return GrossLosEdge::None;
    s.gross        = false;
    s.clear_streak = 0;
    s.over_streak  = 0;
    return GrossLosEdge::Clear;
}

// ---------------------------------------------------------------------------
// The actuator's publish half (serial, pre-assembly)
// ---------------------------------------------------------------------------

void networkGrossLosPublishScales(NetworkRunContext& ctx,
                                  std::vector<NetStation>& stations) {
    if (!ctx.knobs.gl_on) return;   // OFF: the map is never populated ⇒ byte-identical
    const double f = ctx.knobs.gl_deweight;
    // Rebuild from scratch: a released or expired line of sight must LOSE its
    // scale in the same pass that a new declaration gains one, and a swap-based
    // rebuild is the only version of that with no stale-key window.
    std::vector<std::map<std::string, double>> fresh(stations.size());
    for (const auto& [key, st] : ctx.carry.gl_state) {
        if (!st.gross) continue;
        const std::size_t i = static_cast<std::size_t>(key.first);
        if (i >= fresh.size()) continue;
        fresh[i][key.second] = f;
    }
    for (std::size_t i = 0; i < stations.size(); ++i)
        stations[i].gl_code_sigma_scale.swap(fresh[i]);
}

// ---------------------------------------------------------------------------
// The seam
// ---------------------------------------------------------------------------

void networkGrossLosScreen(NetworkRunContext& ctx,
                           const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           NetworkRunTraces& traces,
                           const SyncedEpoch& se,
                           std::vector<NetStation>& stations,
                           const NetworkEstimator& est,
                           const Eigen::VectorXd& x_s,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           double now_s) {
    if (!ctx.knobs.gl_on) return;

    const NetworkConfig& nc = config_.getNetworkConfig();
    const double k            = nc.gross_los_k;
    const int    min_rows     = nc.gross_los_min_rows;
    const int    dec_epochs   = nc.gross_los_epochs;
    const double clear_sigma  = nc.gross_los_clear_sigma;
    const int    clr_epochs   = nc.gross_los_clear_epochs;
    const double max_gap_s    = nc.gross_los_max_gap_s;
    const int    holdoff      = nc.gross_los_event_holdoff_ep;
    const int    warmup_ep    = nc.gross_los_warmup_epochs;
    const double max_sta_frac = nc.gross_los_max_station_frac;

    // --- GUARD 1: the MATURITY horizon (invariant #4's other half — a tier that
    // fires on ignition is measuring ignition).  MEASURED on the cert smoke: with
    // no horizon the screen declared 520 lines of sight in 20 cold epochs, its
    // deweight removed the code information the cold solve needs for the receiver
    // clocks, post-fit code sigma-hat-0 went 3.2 -> 2334 and the SH health gate
    // withheld 9 of 20 epochs.  The screen still SCREENS during warmup — every
    // counter and the whole |v|/sigma census are live — it only does not DECLARE,
    // so the run says exactly what it would have done.  Same guard, same reason,
    // as the RAIM isolated-arc refloat's networkWarmupRampLifted.
    const bool mature = ctx.counters.n_emitted >= static_cast<long long>(warmup_ep);

    auto& tal    = ctx.counters.gross_los;
    auto& gstate = ctx.carry.gl_state;
    ++tal.epochs;
    ++ctx.carry.gl_seq;
    const long long seq = ctx.carry.gl_seq;

    auto emitEvent = [&](const char* event, const std::string& sat,
                         const std::string& station, std::optional<double> statistic,
                         std::optional<double> threshold, const char* action,
                         const std::string& detail) {
        if (holdoff > 0) {
            const std::string hk = station + "\x1f" + sat;
            auto& last = ctx.carry.gl_event_last;
            const auto it = last.find(hk);
            if (it != last.end() && (seq - it->second) < holdoff) return;
            last[hk] = seq;
        }
        traces.emitEvent(se.time, "GROSSLOS", event, sat, station, statistic,
                         threshold, action, detail);
    };

    // --- Shared-column families ------------------------------------------------
    // Identical rule and identical label source to networkPostfitSigma0's bitmap
    // pass (NetworkFrameDiag.cpp), so the acting screen and the measuring trace
    // can never disagree about which rows are code.
    std::vector<char> is_sh_col, is_pb_col, is_cb_col, is_clk_col;
    std::vector<std::string> col_sat, col_sig;
    if (x_s.size() > 0) {
        const std::size_t nsh = static_cast<std::size_t>(x_s.size());
        is_sh_col.assign(nsh, 0);
        is_pb_col.assign(nsh, 0);
        is_cb_col.assign(nsh, 0);
        is_clk_col.assign(nsh, 0);
        col_sat.assign(nsh, {});
        col_sig.assign(nsh, {});
        for (Eigen::Index c = 0; c < x_s.size(); ++c) {
            std::string lbl;
            try { lbl = est.sharedColumnLabel(c); } catch (...) { continue; }
            const std::size_t u = static_cast<std::size_t>(c);
            if      (lbl.rfind("SH", 0)  == 0) { is_sh_col[u] = 1; continue; }
            else if (lbl.rfind("PB:", 0) == 0) is_pb_col[u] = 1;
            else if (lbl.rfind("CB:", 0) == 0) is_cb_col[u] = 1;
            else if (lbl.rfind("CLK:", 0) == 0) is_clk_col[u] = 1;
            // "KIND:sat[:signal]" — sat between the first and (optional) second colon.
            const std::size_t p1 = lbl.find(':');
            if (p1 == std::string::npos) continue;
            const std::size_t p2 = lbl.find(':', p1 + 1);
            col_sat[u] = lbl.substr(p1 + 1,
                                    (p2 == std::string::npos) ? std::string::npos : p2 - p1 - 1);
            if (p2 != std::string::npos) col_sig[u] = lbl.substr(p2 + 1);
        }
    }

    // (station index, satellite) seen with at least one code row THIS epoch — the
    // absence half of the streak rule needs the complement of this set.
    std::set<std::pair<int, std::string>> seen;
    int n_declared_ep = 0, n_cleared_ep = 0;

    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != x_s.size() || R.y.size() == 0) continue;

        Eigen::VectorXd x_r;
        try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
        catch (...) { continue; }
        if (x_r.size() != R.A_r.cols()) continue;

        const Eigen::VectorXd v = R.y - R.A_s * x_s - R.A_r * x_r;

        // Local columns carrying a present ambiguity — the PHASE discriminator.
        // The pool is whichever census this lineage fills: pl_amb on the leveling
        // vehicle, qc_amb on a Card-4 one.  This is a CLASSIFICATION read, not a
        // fault pool: its only job is to keep phase rows out of the code bins.
        std::vector<char> is_amb_col(static_cast<std::size_t>(R.A_r.cols()), 0);
        for (const auto& pa : (R.qc_amb.empty() ? R.pl_amb : R.qc_amb))
            if (pa.col >= 0 && pa.col < R.A_r.cols())
                is_amb_col[static_cast<std::size_t>(pa.col)] = 1;

        std::map<std::string, std::vector<GrossLosSample>> by_sat;
        for (Eigen::Index row = 0; row < v.size(); ++row) {
            // Card-1 coupling rows are statements about the CARRY, not data.
            if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) continue;
            bool sh_row = false, pb_row = false, cb_row = false, clk_row = false;
            const std::string* row_sat = nullptr;
            const std::string* row_sig = nullptr;
            for (Eigen::Index c = 0; c < R.A_s.cols(); ++c) {
                if (R.A_s(row, c) == 0.0) continue;
                const std::size_t u = static_cast<std::size_t>(c);
                if (is_sh_col[u]) { sh_row = true; break; }
                if (is_pb_col[u]) pb_row = true;
                else if (is_cb_col[u]) cb_row = true;
                else if (is_clk_col[u]) clk_row = true;
                if (!row_sat && !col_sat[u].empty()) row_sat = &col_sat[u];
                if (!row_sig && !col_sig[u].empty()) row_sig = &col_sig[u];
            }
            if (sh_row) continue;     // the SH↔slant-iono tie is a documented 3σ artifact
            bool phase_row = pb_row;
            for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                if (is_amb_col[static_cast<std::size_t>(c)] && std::abs(R.A_r(row, c)) > 0.5)
                    phase_row = true;
            if (phase_row) continue;                        // the phase side has its own tiers
            if (!(cb_row || clk_row) || !row_sat) continue;  // no satellite partial ⇒ not a LOS row

            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double r = std::abs(v(row)) * std::sqrt(std::max(0.0, w));
            if (!std::isfinite(r)) continue;
            by_sat[*row_sat].push_back(
                GrossLosSample{ row_sig ? *row_sig : std::string(), r, v(row) });
        }
        if (by_sat.empty()) continue;
        ++tal.stations_screened;

        // Assess EVERY line of sight of the station before acting on any of them:
        // guard 2 needs the station's whole over-bar fraction, and a decision taken
        // on the first satellite of an alphabetical walk would depend on which
        // satellite that happened to be.
        std::map<std::string, GrossLosVerdict> verdicts;
        int n_los_here = 0, n_over_here = 0;
        for (const auto& [sat, rows] : by_sat) {
            ++tal.los_screened;
            tal.rows_screened += static_cast<long long>(rows.size());
            const GrossLosVerdict d =
                networkGrossLosAssess(rows, k, min_rows, clear_sigma);
            if (d.r_max > tal.r_max) tal.r_max = d.r_max;
            if (d.v_abs_max > tal.v_abs_max_m) tal.v_abs_max_m = d.v_abs_max;
            if (d.over) { ++tal.los_over; ++n_over_here; }
            ++n_los_here;
            verdicts.emplace(sat, d);
        }
        // --- GUARD 2: the WHOLE-STATION stand-down --------------------------
        // A gross line of sight is by definition ISOLATED.  When most of a
        // station's lines of sight are over the bar the fault is the STATION's —
        // on `runs/_cm_c10` at hour 5 that is BSHM00ISR, whose ZWD state has
        // diverged to 197 m with every one of its ten satellites past 20 m — and
        // de-weighting every code row of that station removes exactly the
        // information that determines its clock and its troposphere.  This screen
        // cannot fix a station; standing down and SAYING so is the honest
        // response, and the alternative is what the cert smoke measured.
        // Measured shape on c10: the per-station over-bar fraction is p90 = 0.00
        // in hours 1-4 and 0.11-0.12 in the two worst hours, with a max reaching
        // 0.88-1.00 — so a 0.5 cap fires on the collapse and never on an isolated
        // line of sight.  CLEARS are deliberately NOT blocked (see the step's
        // contract): a guard that froze the release edge would strand the
        // response with no way out.
        // Evaluated only once the MATURITY horizon has opened: before that nothing
        // can be declared anyway, so the stand-down has nothing to guard, and
        // emitting it through a cold start would fill the event stream with a
        // population that is the ignition transient rather than a station fault.
        const bool sta_standdown =
            mature && max_sta_frac < 1.0 && n_los_here > 0 &&
            static_cast<double>(n_over_here) > max_sta_frac * static_cast<double>(n_los_here);
        if (sta_standdown) {
            ++tal.standdowns;
            ++tal.standdown_by_station[stations[i].name];
            emitEvent("gross_los_station_standdown", "", stations[i].name,
                      static_cast<double>(n_over_here) / static_cast<double>(n_los_here),
                      max_sta_frac, "standdown",
                      fmt::format("n_over={};n_los={};reason=whole_station_not_one_los",
                                  n_over_here, n_los_here));
        }

        for (const auto& [sat, d] : verdicts) {
            const auto key = std::make_pair(static_cast<int>(i), sat);
            seen.insert(key);
            GrossLosState& st = gstate[key];
            st.last_seen_s = now_s;
            const bool allow = mature && !sta_standdown;
            const GrossLosEdge edge =
                networkGrossLosStep(st, d, dec_epochs, clr_epochs, se.time.tow, allow);
            if (edge == GrossLosEdge::Suppressed) {
                if (!mature) ++tal.suppressed_warmup;
                else         ++tal.suppressed_station;
                continue;
            }

            if (edge == GrossLosEdge::Declare) {
                ++tal.declared;
                ++n_declared_ep;
                ++tal.declared_by_station[stations[i].name];
                ++tal.declared_by_sat[sat];
                // --- THE RESPONSE (decoupling edge) --------------------------
                // Exactly the T4 attribution-erase vocabulary, so the two
                // responses cannot drift.  Every step is idempotent: a line of
                // sight RAIM already refloated this epoch is unharmed by a
                // second visit.
                int n_amb = 0;
                for (const auto& pa : (R.qc_amb.empty() ? R.pl_amb : R.qc_amb)) {
                    if (pa.sat != sat) continue;
                    if (ctx.carry.local_carry.erase(
                            std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)))
                        ++ctx.counters.n_cr_gross_los;
                    // BOOKKEEPING on a Card-4 lineage and THE actuator on the
                    // phase-leveling one — the same asymmetry the RAIM erase
                    // documents.  Marked unconditionally for the same reason it
                    // is there: the leveling vehicle is where this screen runs.
                    if (stations[i].sm) stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                    ++n_amb;
                }
                tal.amb_refloated += n_amb;
                // The LOS's slant iono was estimated jointly with a level this
                // screen has just stopped trusting, and the code residual it
                // fired on is exactly the channel that feeds it — so the iono
                // carry cannot vouch for itself either.  Erasing it makes the
                // line of sight re-draw its slant iono from the SH tie (the
                // field value at its pierce point) plus this epoch's data, which
                // is what §24.18's "re-seed its per-LOS iono from the SH field"
                // is in this producer's vocabulary: there is no separate write
                // path for a per-LOS iono value, and inventing one would be a
                // second mechanism where the carry erase already reaches the
                // same state through the sanctioned route.
                if (ctx.carry.iono_carry.erase({static_cast<int>(i), sat})) ++tal.iono_erased;
                if (ctx.carry.iono_blk_carry.erase({static_cast<int>(i), sat})) ++tal.iono_erased;
                emitEvent("gross_los_declare", sat, stations[i].name, d.r_max, k, "declare",
                          fmt::format("n_over={};n_rows={};v_abs_max_m={:.3f};sigs={};"
                                      "amb_refloat={};episode={};deweight_sigma_x={:g}",
                                      d.n_over, d.n_rows, d.v_abs_max,
                                      d.signals.empty() ? "-" : d.signals,
                                      n_amb, st.n_declared, ctx.knobs.gl_deweight));
            } else if (edge == GrossLosEdge::Clear) {
                ++tal.cleared;
                ++n_cleared_ep;
                emitEvent("gross_los_clear", sat, stations[i].name, d.r_max, clear_sigma,
                          "clear",
                          fmt::format("n_rows={};v_abs_max_m={:.3f};declared_tow={:.1f};"
                                      "episode={}",
                                      d.n_rows, d.v_abs_max, st.declared_tow, st.n_declared));
            }
        }
    }

    // --- Absence: the streak reset and the ABSENT-SATELLITE edge ----------------
    // Standing invariant #1: a declared line of sight that stops being observed
    // must not keep its response indefinitely — the next sighting is a new arc
    // with a new ambiguity, and the old evidence cannot speak for it.
    for (auto it = gstate.begin(); it != gstate.end(); ) {
        const bool here = seen.count(it->first) != 0;
        if (here) { ++it; continue; }
        it->second.over_streak  = 0;   // absence is not evidence either way
        it->second.clear_streak = 0;
        const double gap = now_s - it->second.last_seen_s;
        if (gap <= max_gap_s) { ++it; continue; }
        if (it->second.gross) {
            ++tal.expired;
            const std::size_t si = static_cast<std::size_t>(it->first.first);
            emitEvent("gross_los_expire", it->first.second,
                      si < stations.size() ? stations[si].name : std::string(),
                      gap, max_gap_s, "expire",
                      fmt::format("unseen_s={:.1f};declared_tow={:.1f};episode={}",
                                  gap, it->second.declared_tow, it->second.n_declared));
        }
        it = gstate.erase(it);
    }

    long long now_gross = 0;
    for (const auto& kv : gstate) if (kv.second.gross) ++now_gross;
    tal.currently_gross = now_gross;
    if (now_gross > tal.max_concurrent) tal.max_concurrent = now_gross;

    // Republish the assembly's code-scale map from the set this epoch leaves
    // behind.  SERIAL and immediately after the decision, so the next epoch's
    // parallel produce reads a settled map (the phase_sigma_scale contract).
    networkGrossLosPublishScales(ctx, stations);

    if (n_declared_ep > 0 || n_cleared_ep > 0)
        log->info("Network (gross-LOS screen) TOW {:.1f}: {} declared, {} cleared, "
                  "{} currently gross (run-max standardized code residual so far {:.1f} sigma)",
                  se.time.tow, n_declared_ep, n_cleared_ep, now_gross, tal.r_max);
}

} // namespace GnssProcessor
