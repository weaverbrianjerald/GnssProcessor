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
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// The PURE rules (unit-tested in tests/test_network_gross_los.cpp)
// ---------------------------------------------------------------------------

double networkGrossLosUndoDeweight(double r_folded, double scale) {
    return (std::isfinite(scale) && scale > 1.0) ? r_folded * scale : r_folded;
}

GrossLosVerdict networkGrossLosAssess(const std::vector<GrossLosSample>& rows,
                                      double k, int min_rows, double clear_sigma,
                                      bool min_rows_available) {
    GrossLosVerdict d;
    d.n_rows = static_cast<int>(rows.size());
    d.min_rows_eff = std::max(1, min_rows);
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
    // RF-3b B1 (requirement F1): the bar as a RULE.  `fixed` is today — at least
    // min_rows rows over k, whatever the line of sight has, which is unsatisfiable when
    // it has fewer rows than the constant.  `available` reads it as
    // min(min_rows, the LOS's own code rows), so a one-row line of sight is reachable
    // while a three-row one still needs two: the multi-band corroboration the constant
    // exists for is kept exactly where the line of sight can supply it.
    const int bar_fixed = std::max(1, min_rows);
    const int bar_avail = std::max(1, std::min(bar_fixed, d.n_rows));
    d.min_rows_eff = min_rows_available ? bar_avail : bar_fixed;
    d.over = d.n_over >= d.min_rows_eff;
    // B5 (requirement F6): the population the FIXED rule cannot reach, counted under
    // both rules so an arm that does not arm B1 still reports what B1 would have
    // reached.  It is exactly "the evidence clears the available bar and the line of
    // sight is SHORT" — four of the six largest episodes on runs/_rf0hv9_field6.
    d.short_over = (d.n_rows < bar_fixed) && (d.n_over >= bar_avail);
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
    // RF-3b B4 (requirement F4): a STATION-level declaration folds EVERY one of that
    // station's code rows.  The assembly's map is keyed per satellite — it is the same
    // map the per-LOS response writes — so a station-wide fold is expressed as "every
    // satellite this station was last seen screening".  That set is refreshed each
    // screened epoch, so a satellite that rises while the station is declared is folded
    // from the epoch it is first seen, and one that sets stops being carried.
    for (const auto& [si, sst] : ctx.carry.gl_sta_state) {
        if (!sst.gross) continue;
        const std::size_t i = static_cast<std::size_t>(si);
        if (i >= fresh.size()) continue;
        for (const auto& sat : sst.sats) fresh[i][sat] = f;
    }
    for (std::size_t i = 0; i < stations.size(); ++i)
        stations[i].gl_code_sigma_scale.swap(fresh[i]);
}

// ---------------------------------------------------------------------------
// RF-3b B2: the HOLD — the bounded decoupling edge (standing invariant #7)
// ---------------------------------------------------------------------------
namespace {

/// RF-3b B2: freeze one carried entry at its pre-declaration value.  The border
/// regression and the joint block are cleared (that half IS the decoupling: a
/// regression harvested against a level the screen has stopped trusting would keep
/// injecting that level's relationship to the shared border at full strength), while
/// the VALUE and the VARIANCE are kept exactly as they were.  With q = 0 on the
/// ambiguity block a prior that is re-written unchanged after every harvest is
/// literally "a fixed prior with zero process noise".
LocalCarry glFreeze(const LocalCarry& src) {
    LocalCarry h = src;
    h.coupling.clear();
    h.blk.reset();
    h.blk_idx = -1;
    return h;
}

}  // namespace

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
                           double now_s,
                           GrossLosPhase phase) {
    if (!ctx.knobs.gl_on) return;
    // RF-3b B3: the ACT half is the deferred response of this epoch's DECIDE pass.  It
    // is a separate entry point so the driver has one call shape at both sites.
    if (phase == GrossLosPhase::Act) {
        networkGrossLosAct(ctx, log, stations, asm_res);
        return;
    }

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
    // --- GL-2 (docs/SPEC_event_harness.md §24.19): the WHOLE-SATELLITE stand-down.
    const bool   gl2_on       = ctx.knobs.gl2_on;
    const int    gl2_min_sta  = nc.gross_los_sat_min_stations;
    const bool   gl2_wire     = nc.gross_los_sat_wire_withhold;

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

    // --- RF-3b sub-rules.  Each already ANDed with gl_on at the knob resolve, so an
    // unarmed run reads exactly today's constants and today's path.
    const bool   mrr_avail    = ctx.knobs.gl_min_rows_available;
    const bool   gl_bound     = ctx.knobs.gl_bound;
    const bool   gl_emit_excl = ctx.knobs.gl_emit_exclude;
    const bool   gl_sta_tier  = ctx.knobs.gl_station_tier;

    auto& tal    = ctx.counters.gross_los;
    auto& gstate = ctx.carry.gl_state;
    ++tal.epochs;
    ++ctx.carry.gl_seq;
    const long long seq = ctx.carry.gl_seq;
    // RF-3b B3: this epoch's deferred work.  Rebuilt every screened epoch, so a release
    // stops being carried in the same pass a new declaration starts.
    ctx.carry.gl_act_declare.clear();
    ctx.carry.gl_act_release.clear();
    ctx.carry.gl_act_station.clear();
    ctx.carry.gl_emit_withhold.clear();
    ctx.carry.gl_emit_disc.clear();

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

    // GL-2 needs a NETWORK-WIDE tally before any line of sight is acted on: "the same
    // satellite over the bar at >= N stations in one epoch" is not knowable inside a
    // per-station walk, and a decision taken on the first station of that walk would
    // depend on which station it happened to be.  So the pass is split: ASSESS every
    // station, TALLY per satellite, then ACT.  The store is ~61 x 10 verdicts.
    struct StationPass {
        std::size_t i{0};
        std::map<std::string, GrossLosVerdict> verdicts;
        bool sta_standdown{false};
    };
    std::vector<StationPass> passes;
    passes.reserve(stations.size());
    std::map<std::string, int> sat_over_stations;   // satellite -> stations over the bar

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

        // The scale map the ASSEMBLY read for this epoch — this seam republishes
        // it only at its own end, so what is here now is exactly what was folded.
        // Used to undo the fold in the statistic; see the row build below.
        const std::map<std::string, double>& gl_scale = stations[i].gl_code_sigma_scale;

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

            double gl_scale_here = 1.0;
            if (!gl_scale.empty()) {
                const auto sc = gl_scale.find(*row_sat);
                if (sc != gl_scale.end()) gl_scale_here = sc->second;
            }
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            // ⚠ THE RESPONSE MUST NOT LAUNDER ITS OWN RELEASE TEST.
            // A declared line of sight's code rows were folded THIS epoch at
            // σ × f, so |v|·√w reads f times SMALLER than the same residual at
            // nominal weight.  At f = 100 a 144 m residual reads 1.3 σ — under
            // the 3 σ release bar, one epoch after the same line of sight was
            // declared at 125 σ.  MEASURED on the first `runs/_gl_on6`:
            // declare TOW 265500 (125.6 σ, |v| 143.8 m) → clear 265560
            // (1.28 σ, |v| STILL 144.7 m) → declare 265620 → clear 265680: a
            // two-epoch limit cycle that erases the LOS's carry forever and
            // leaves the deweight on only half the time.
            // Undo the fold EXACTLY.  `gl_code_sigma_scale` is the very map the
            // assembly read for this epoch (this seam republishes it only at
            // its end), so multiplying by it restores the row's nominal σ and
            // both bars then measure the DATA rather than the response.
            const double r = networkGrossLosUndoDeweight(
                std::abs(v(row)) * std::sqrt(std::max(0.0, w)), gl_scale_here);
            if (!std::isfinite(r)) continue;
            // RF-3b B5 (requirement F6): the F2 alarm — how far a row that is ACTUALLY
            // FOLDED has run.  A folded row's residual is supposed to stay a MEASURE of
            // the fault; on `runs/_rap_rf3_gl1` it ran to 445.48 m, which is what an
            // unbounded decoupling edge looks like from the outside.  Counted whether or
            // not network.gross_los_bound is armed, so the two arms are comparable.
            if (gl_scale_here > 1.0) {
                const double av_f = std::abs(v(row));
                if (std::isfinite(av_f) && av_f > tal.folded_v_abs_max_m)
                    tal.folded_v_abs_max_m = av_f;
            }
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
                networkGrossLosAssess(rows, k, min_rows, clear_sigma, mrr_avail);
            if (d.r_max > tal.r_max) tal.r_max = d.r_max;
            if (d.v_abs_max > tal.v_abs_max_m) tal.v_abs_max_m = d.v_abs_max;
            if (d.over) { ++tal.los_over; ++n_over_here; }
            // RF-3b B5: B1's own population, counted under BOTH rules (the verdict fills
            // it either way) — LOS-epochs whose evidence clears the available bar while
            // the line of sight has fewer code rows than the configured constant.  With
            // the fixed rule this is exactly what the screen CANNOT reach.
            if (d.short_over) ++tal.los_over_short;
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
        // --- RF-3b B4 (requirement F4): the STATION TIER --------------------
        // The guard above is correct about the diagnosis and does nothing with it: it
        // holds THIS epoch's declarations and the next epoch declares the same lines of
        // sight one by one on the same evidence, which is how a station fault becomes N
        // line-of-sight faults (BSHM00ISR: guard at TOW 269310 with n_over=5 of n_los=7,
        // per-LOS declaration at 269340).  With the tier armed the guard becomes a
        // VERDICT with its own two edges: every one of the station's code rows folds at
        // the deweight, its freed states are held exactly as a per-LOS declaration's are
        // (under network.gross_los_bound), and it is RELEASED when its over-bar fraction
        // sits at or under the cap for gross_los_clear_epochs consecutive screened
        // epochs.  The release bar is deliberately the CAP and not gross_los_clear_sigma:
        // the station-level statistic IS the fraction, so its hysteresis is the streak.
        if (gl_sta_tier && mature) {
            auto& sst = ctx.carry.gl_sta_state[static_cast<int>(i)];
            sst.sats.clear();
            for (const auto& [sat, d] : verdicts) { (void)d; sst.sats.insert(sat); }
            if (!sst.gross && sta_standdown) {
                sst.gross        = true;
                sst.clear_streak = 0;
                sst.declared_tow = se.time.tow;
                ++sst.n_declared;
                ++tal.sta_declared;
                ++tal.sta_declared_by_station[stations[i].name];
                ctx.carry.gl_act_station.push_back({ static_cast<int>(i), true });
                emitEvent("gross_los_station_declare", "", stations[i].name,
                          static_cast<double>(n_over_here) / static_cast<double>(n_los_here),
                          max_sta_frac, "station_declare",
                          fmt::format("n_over={};n_los={};n_sats={};deweight_sigma_x={:g};"
                                      "episode={};reason=whole_station_not_one_los",
                                      n_over_here, n_los_here, sst.sats.size(),
                                      ctx.knobs.gl_deweight, sst.n_declared));
            } else if (sst.gross) {
                ++tal.sta_los_epochs;
                if (sta_standdown) sst.clear_streak = 0;
                else               ++sst.clear_streak;
                if (sst.clear_streak >= std::max(1, clr_epochs)) {
                    sst.gross        = false;
                    sst.clear_streak = 0;
                    ++tal.sta_released;
                    ctx.carry.gl_act_station.push_back({ static_cast<int>(i), false });
                    emitEvent("gross_los_station_release", "", stations[i].name,
                              static_cast<double>(n_over_here)
                                  / static_cast<double>(n_los_here),
                              max_sta_frac, "station_release",
                              fmt::format("n_over={};n_los={};declared_tow={:.1f};episode={}",
                                          n_over_here, n_los_here, sst.declared_tow,
                                          sst.n_declared));
                }
            }
        }

        if (gl2_on)
            for (const auto& [sat, d] : verdicts)
                if (d.over) ++sat_over_stations[sat];
        passes.push_back(StationPass{ i, std::move(verdicts), sta_standdown });
    }

    // --- GL-2: the WHOLE-SATELLITE stand-down (§24.19) ------------------------
    // MEASURED on `runs/_gl_on6` (§24.18's arm): at TOW 271200/271260 the screen
    // declared TEN lines of sight and every one of them was the SAME satellite,
    // G17, at SEYG00SYC / NYA200NOR / CEBR00ESP / LEIJ00DEU / MIZU00JPN /
    // LCK400IND / KIT300UZB / ULAB00MNG / URUM00CHN / SVTL00RUS — ten stations
    // spread over four continents, residuals 4–49 m.  A satellite whose code is
    // wrong at ten stations at once is not ten gross lines of sight; it is a bad
    // SATELLITE (its clock, its orbit or its code bias).  De-weighting it at ten
    // stations removes it from the code solution network-wide, and the held-out
    // consumer went to 56 m and decayed for an hour.
    //
    // This is the same rule as the whole-station stand-down, one axis over, and it
    // is the population post-fit RAIM's cross-station CONSENSUS already exists to
    // route (`network.raim_min_stations`, default 3: "a satellite so flagged at >=
    // 3 stations is refloated network-wide").  GL-2 therefore does not invent a
    // response — it routes the satellite to the two mechanisms the producer already
    // owns for a consensus fault:
    //   * the per-satellite RETRO DISC BUMP, by queueing the satellite into
    //     `ctx.carry.ura_retro_sats` — the same queue `NetworkSolPrep` drains into
    //     `emitter.markSatDiscontinuity`, with that seam's mark-==-land semantics
    //     (a satellite absent from an emission stays pending) and its RETRO event;
    //   * the per-satellite WIRE WITHHOLD, by queueing it into
    //     `ctx.carry.gl2_wire_pending`, which `NetworkSolPrep` prunes out of
    //     `sol.sats` exactly the way the RT0-5 runaway quarantine and the RTA-10
    //     per-constellation carve-out prune theirs.  NEVER an epoch withhold: the
    //     epoch still emits every other satellite (adapt-and-emit).
    // Both are one epoch late, which is the lag every post-fit tier here has (the
    // emit seams run before SB_RAIM), and is exactly why the consensus path was
    // built edge-triggered and pending-aware in the first place.
    std::set<std::string> sat_down_now;
    if (gl2_on && mature)
        for (const auto& [sat, n] : sat_over_stations)
            if (n >= gl2_min_sta) sat_down_now.insert(sat);

    if (gl2_on) {
        // The DECOUPLING edge: a satellite that was not stood down and now is.
        for (const auto& sat : sat_down_now) {
            if (ctx.carry.gl2_sat_down.count(sat)) continue;
            ctx.carry.gl2_sat_down.insert(sat);
            ctx.carry.gl2_sat_since[sat] = se.time.tow;
            ++tal.sat_standdowns;
            ++tal.sat_standdown_by_sat[sat];
            emitEvent("gross_los_sat_standdown", sat, "",
                      static_cast<double>(sat_over_stations[sat]),
                      static_cast<double>(gl2_min_sta), "standdown",
                      fmt::format("n_sta_over={};min_sta={};reason=whole_satellite_not_one_los;"
                                  "routed={}",
                                  sat_over_stations[sat], gl2_min_sta,
                                  gl2_wire ? "retro_disc+wire_withhold" : "retro_disc"));
        }
        // The RE-COUPLING edge: a satellite that was stood down and no longer is.
        for (auto it = ctx.carry.gl2_sat_down.begin(); it != ctx.carry.gl2_sat_down.end(); ) {
            if (sat_down_now.count(*it)) { ++it; continue; }
            const auto sit = ctx.carry.gl2_sat_since.find(*it);
            ++tal.sat_released;
            emitEvent("gross_los_sat_release", *it, "",
                      static_cast<double>(sat_over_stations.count(*it)
                                              ? sat_over_stations[*it] : 0),
                      static_cast<double>(gl2_min_sta), "release",
                      fmt::format("n_sta_over={};standdown_tow={:.1f}",
                                  sat_over_stations.count(*it) ? sat_over_stations[*it] : 0,
                                  sit != ctx.carry.gl2_sat_since.end() ? sit->second : 0.0));
            if (sit != ctx.carry.gl2_sat_since.end()) ctx.carry.gl2_sat_since.erase(sit);
            it = ctx.carry.gl2_sat_down.erase(it);
        }
        // Route the CURRENTLY stood-down set to the producer's own consensus-fault
        // response, rebuilt every epoch so a released satellite stops being routed
        // in the same pass a newly-faulted one starts.
        ctx.carry.gl2_wire_pending.clear();
        if (!ctx.carry.gl2_sat_down.empty()) {
            ++tal.sat_epochs;
            tal.sat_epoch_max = std::max<long long>(
                tal.sat_epoch_max, static_cast<long long>(ctx.carry.gl2_sat_down.size()));
            for (const auto& sat : ctx.carry.gl2_sat_down) {
                ctx.carry.ura_retro_sats.insert(sat);   // the consensus path's disc bump
                if (gl2_wire) ctx.carry.gl2_wire_pending.insert(sat);
            }
        }
    }

    // --- ACT ------------------------------------------------------------------
    for (auto& pass : passes) {
        const std::size_t i = pass.i;
        const bool sta_standdown = pass.sta_standdown;
        for (const auto& [sat, d] : pass.verdicts) {
            const auto key = std::make_pair(static_cast<int>(i), sat);
            seen.insert(key);
            GrossLosState& st = gstate[key];
            st.last_seen_s = now_s;
            // GL-2: a stood-down SATELLITE takes its lines of sight with it.  An
            // already-declared one is RELEASED here — the deweight must lift, because
            // the fault is not this line of sight's and holding it decoupled would keep
            // removing code the solution needs (standing invariant #7's re-coupling
            // edge, at the satellite's scale).  A release is a real edge and is
            // counted; it deliberately does NOT go through the per-LOS clear event,
            // which asserts the DATA healed — this one did not.
            const bool sat_down = gl2_on && ctx.carry.gl2_sat_down.count(sat) > 0;
            if (sat_down && st.gross) {
                st.gross = false;
                st.over_streak = 0;
                st.clear_streak = 0;
                ++tal.sat_los_released;
                // RF-3b B2: the hold is the response's state, so it lifts with the
                // response — a released line of sight must stop being frozen.
                ctx.carry.gl_act_release.push_back({ static_cast<int>(i), sat });
            }
            // RF-3b B5: the per-episode latency's ORIGIN.  Stamped when an over-bar
            // streak OPENS, cleared when it ends, so a declaration can state how long
            // the evidence stood rather than only when the response acted.
            if (d.over) {
                if (st.over_streak == 0 && !st.gross) {
                    st.first_over_tow    = se.time.tow;
                    st.first_over_age_ep = 0;
                } else {
                    ++st.first_over_age_ep;
                }
            } else if (!st.gross) {
                st.first_over_tow    = 0.0;
                st.first_over_age_ep = 0;
            }
            // RF-3b B4: a STATION-level declaration replaces the per-LOS ones — that is
            // the whole point of the tier, so it forbids new per-LOS declarations at
            // that station for as long as it stands, exactly as the guard does for one
            // epoch.  Releases stay allowed (the step's own contract).
            const bool sta_declared_now =
                gl_sta_tier && ctx.carry.gl_sta_state.count(static_cast<int>(i))
                && ctx.carry.gl_sta_state[static_cast<int>(i)].gross;
            const bool allow =
                mature && !sta_standdown && !sat_down && !sta_declared_now;
            const GrossLosEdge edge =
                networkGrossLosStep(st, d, dec_epochs, clr_epochs, se.time.tow, allow);
            if (edge == GrossLosEdge::Suppressed) {
                if (!mature)        ++tal.suppressed_warmup;
                else if (sat_down)  ++tal.sat_los_suppressed;
                else                ++tal.suppressed_station;
                continue;
            }

            if (edge == GrossLosEdge::Declare) {
                ++tal.declared;
                ++n_declared_ep;
                ++tal.declared_by_station[stations[i].name];
                ++tal.declared_by_sat[sat];
                // --- THE RESPONSE (decoupling edge) --------------------------
                // RF-3b B3: the response is DEFERRED here and applied by
                // @ref networkGrossLosAct, which the Full phase calls at the end of
                // this same pass (today's behaviour, byte for byte) and the split
                // phases call after the carry harvest.  The ordering reason is
                // unchanged: erasing or freezing a local_carry entry before the
                // harvest would simply be re-stashed.
                ctx.carry.gl_act_declare.push_back({ static_cast<int>(i), sat });
                // RF-3b B5: the per-episode latency, in screened epochs, from the
                // first row over the bar to the declaration.
                const long long lat = static_cast<long long>(st.first_over_age_ep);
                tal.declare_latency_sum_ep += lat;
                if (lat > tal.declare_latency_max_ep) tal.declare_latency_max_ep = lat;
                emitEvent("gross_los_declare", sat, stations[i].name, d.r_max, k, "declare",
                          fmt::format("n_over={};n_rows={};min_rows_eff={};"
                                      "v_abs_max_m={:.3f};sigs={};episode={};"
                                      "deweight_sigma_x={:g};response={};latency_ep={};"
                                      "first_over_tow={:.1f};wire_latency_ep={}",
                                      d.n_over, d.n_rows, d.min_rows_eff, d.v_abs_max,
                                      d.signals.empty() ? "-" : d.signals,
                                      st.n_declared, ctx.knobs.gl_deweight,
                                      gl_bound ? "hold" : "erase", lat,
                                      st.first_over_tow, gl_emit_excl ? 0 : 1));
                st.first_over_age_ep = 0;
            } else if (edge == GrossLosEdge::Clear) {
                ++tal.cleared;
                ++n_cleared_ep;
                ctx.carry.gl_act_release.push_back({ static_cast<int>(i), sat });
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
            // RF-3b B2: the expiry is the other exit from a declaration, so it drops the
            // hold too — otherwise a frozen prior would ride a line of sight the screen
            // has stopped answering for (standing invariant #1's absent-satellite edge).
            ctx.carry.gl_act_release.push_back({ it->first.first, it->first.second });
        }
        it = gstate.erase(it);
    }

    long long now_gross = 0;
    for (const auto& kv : gstate) if (kv.second.gross) ++now_gross;
    tal.currently_gross = now_gross;
    if (now_gross > tal.max_concurrent) tal.max_concurrent = now_gross;

    // --- RF-3b B3 (requirement F3): the EMISSION's contributor set ---------------
    // The screen has always acted one epoch late on the wire: it runs after the emit
    // seams, so the two largest rows of every episode are already emitted when the
    // response lands (products first differ at TOW 269700 for a declaration at 269670).
    // With the seam split, this pass runs BEFORE the emission, and the declared line of
    // sight leaves its satellite's CONTRIBUTOR SET for the epoch it declares.
    //
    // ⚠ WHAT THIS IS NOT.  The emitted clock and code bias are joint-solve SHARED
    // states — NetworkEstimator::toSolutionEpoch copies x_s(c) straight into
    // clock_c0_m / code_bias_m — so there is no per-contributor decomposition to
    // subtract and no aggregation to re-average.  Changing the emitted VALUE without
    // the line of sight is a second network solve of the epoch, which this knob
    // deliberately does not take.  What it does is exact: the satellite's records are
    // WITHHELD when too few contributors remain to stand behind them, and otherwise
    // emitted with the producer's existing per-satellite DISCONTINUITY mark, so the
    // consumer is told the contributor set moved instead of reading a silent step.
    if (gl_emit_excl) {
        std::map<std::string, int> contrib, excluded;
        for (const auto& pass : passes) {
            const int si = static_cast<int>(pass.i);
            const bool sta_down_here =
                gl_sta_tier && ctx.carry.gl_sta_state.count(si)
                && ctx.carry.gl_sta_state[si].gross;
            for (const auto& [sat, d] : pass.verdicts) {
                (void)d;
                ++contrib[sat];
                const auto sit = gstate.find({ si, sat });
                if (sta_down_here || (sit != gstate.end() && sit->second.gross))
                    ++excluded[sat];
            }
        }
        for (const auto& [sat, n_ex] : excluded) {
            if (n_ex <= 0) continue;
            ++tal.emit_excluded;
            const int n_left = contrib[sat] - n_ex;
            if (n_left < gl2_min_sta) {
                ctx.carry.gl_emit_withhold.insert(sat);
                emitEvent("gross_los_emit_withhold", sat, "",
                          static_cast<double>(n_left),
                          static_cast<double>(gl2_min_sta), "drop_sat",
                          fmt::format("n_contrib={};n_excluded={};epoch_still_emits=1;"
                                      "reason=declared_los_left_contributor_set",
                                      contrib[sat], n_ex));
            } else {
                ctx.carry.gl_emit_disc.insert(sat);
                emitEvent("gross_los_emit_disc", sat, "",
                          static_cast<double>(n_left),
                          static_cast<double>(gl2_min_sta), "declare_step",
                          fmt::format("n_contrib={};n_excluded={};"
                                      "reason=declared_los_left_contributor_set",
                                      contrib[sat], n_ex));
            }
        }
    }

    // Republish the assembly's code-scale map from the set this epoch leaves
    // behind.  SERIAL and immediately after the decision, so the next epoch's
    // parallel produce reads a settled map (the phase_sigma_scale contract).
    networkGrossLosPublishScales(ctx, stations);

    // RF-3b B3: the FULL phase applies the deferred response right here, which is the
    // post-harvest site and therefore exactly today's ordering.  Split, the ACT half
    // runs from its own call after the harvest.
    if (phase == GrossLosPhase::Full)
        networkGrossLosAct(ctx, log, stations, asm_res);

    if (n_declared_ep > 0 || n_cleared_ep > 0)
        log->info("Network (gross-LOS screen) TOW {:.1f}: {} declared, {} cleared, "
                  "{} currently gross (run-max standardized code residual so far {:.1f} sigma)",
                  se.time.tow, n_declared_ep, n_cleared_ep, now_gross, tal.r_max);
}

// ---------------------------------------------------------------------------
// RF-3b B3: the deferred RESPONSE (the post-harvest half)
// ---------------------------------------------------------------------------

void networkGrossLosAct(NetworkRunContext& ctx,
                        const std::shared_ptr<spdlog::logger>& log,
                        std::vector<NetStation>& stations,
                        const std::vector<StationAssembly>& asm_res) {
    if (!ctx.knobs.gl_on) return;
    auto& tal      = ctx.counters.gross_los;
    const bool gl_bound = ctx.knobs.gl_bound;

    // --- The per-LOS response.  The two forms are exclusive: OFF is the ERASE the
    //     screen has always applied (a true rebirth — the episode dies within one epoch
    //     of the fold on four of the six measured cases), ON is the HOLD that bounds
    //     what the freed states can accumulate (standing invariant #7's decoupling
    //     edge).  The erase's cost is measured: with only a folded code row left to
    //     determine them, the freed states re-level from an under-determined epoch and
    //     the folded row's residual then runs to 445 m, which the clear test can never
    //     come back from — 2 of 7 declarations ended by expiry rather than release.
    const auto respond = [&](int si, const std::string& sat) {
        const std::size_t i = static_cast<std::size_t>(si);
        if (i >= stations.size() || i >= asm_res.size()) return;
        const StationAssembly& R = asm_res[i];
        int n_amb = 0;
        for (const auto& pa : (R.qc_amb.empty() ? R.pl_amb : R.qc_amb)) {
            if (pa.sat != sat) continue;
            const auto lk = std::make_tuple(si, pa.sat, pa.signal);
            if (gl_bound) {
                const auto lit = ctx.carry.local_carry.find(lk);
                if (lit != ctx.carry.local_carry.end()) {
                    const LocalCarry h = glFreeze(lit->second);
                    ctx.carry.gl_hold_amb[lk] = h;
                    lit->second = h;
                    ++tal.bound_amb_held;
                }
                // NO pending-reseed under the hold: the state is not refloated, it is
                // pinned.  Marking it would hand the next assembly a free column, which
                // is the very thing this rule exists to stop.
            } else {
                if (ctx.carry.local_carry.erase(lk)) ++ctx.counters.n_cr_gross_los;
                // BOOKKEEPING on a Card-4 lineage and THE actuator on the
                // phase-leveling one — the same asymmetry the RAIM erase documents.
                if (stations[i].sm) stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
            }
            // Card SID phase 4a (S1): the GROSS-LOS writer.  The screen's published
            // action is a CODE-row de-weight (a cap, not a drop: the rows still fold),
            // so the arc is recorded with the `deweight` bit, which kArcReasonGapMask
            // excludes from every gap mask by construction.  The `gross` bit records a
            // REMOVAL of carried state, so it is written on the erase path only — a
            // held arc keeps its state and a gap mask must not read it as gone.
            arcReasonRecord(ctx.reasons,
                            ArcLedger::Key{ si, pa.sat, pa.signal }, kArcReasonDeweight);
            if (!gl_bound)
                arcReasonRecord(ctx.reasons,
                                ArcLedger::Key{ si, pa.sat, pa.signal }, kArcReasonGross);
            ++n_amb;
        }
        tal.amb_refloated += n_amb;
        // The LOS's slant iono was estimated jointly with a level this screen has just
        // stopped trusting, and the code residual it fired on is exactly the channel
        // that feeds it — so the iono carry cannot vouch for itself either.  Under the
        // hold it is FROZEN for the same reason the level is: a re-drawn iono is what
        // the folded code row would have to determine, and it cannot.
        const std::pair<int, std::string> ik{ si, sat };
        if (gl_bound) {
            const auto it1 = ctx.carry.iono_carry.find(ik);
            if (it1 != ctx.carry.iono_carry.end()) {
                const LocalCarry h = glFreeze(it1->second);
                ctx.carry.gl_hold_iono[ik] = h;
                it1->second = h;
                ++tal.bound_iono_held;
            }
            const auto it2 = ctx.carry.iono_blk_carry.find(ik);
            if (it2 != ctx.carry.iono_blk_carry.end()) {
                const LocalCarry h = glFreeze(it2->second);
                ctx.carry.gl_hold_iono_blk[ik] = h;
                it2->second = h;
                ++tal.bound_iono_held;
            }
        } else {
            if (ctx.carry.iono_carry.erase(ik))     ++tal.iono_erased;
            if (ctx.carry.iono_blk_carry.erase(ik)) ++tal.iono_erased;
        }
    };

    for (const auto& [si, sat] : ctx.carry.gl_act_declare) respond(si, sat);
    // RF-3b B4: a STATION declaration is the same response over every satellite the
    // station was last seen screening — one verdict, not N sequential ones.
    for (const auto& [si, declared] : ctx.carry.gl_act_station) {
        const auto sit = ctx.carry.gl_sta_state.find(si);
        if (sit == ctx.carry.gl_sta_state.end()) continue;
        if (declared) {
            for (const auto& sat : sit->second.sats) respond(si, sat);
        } else {
            for (const auto& sat : sit->second.sats)
                ctx.carry.gl_act_release.push_back({ si, sat });
        }
    }
    // --- The RE-COUPLING edge: a release or an expiry drops the hold, and the states
    //     continue from the values they were held at.  Idempotent.
    for (const auto& [si, sat] : ctx.carry.gl_act_release) {
        ctx.carry.gl_hold_iono.erase({ si, sat });
        ctx.carry.gl_hold_iono_blk.erase({ si, sat });
        for (auto it = ctx.carry.gl_hold_amb.begin(); it != ctx.carry.gl_hold_amb.end(); ) {
            if (std::get<0>(it->first) == si && std::get<1>(it->first) == sat)
                it = ctx.carry.gl_hold_amb.erase(it);
            else
                ++it;
        }
    }
    // --- RF-3b B2: RE-APPLY the hold over whatever the harvest re-stashed.  This is
    //     the half that makes the prior FIXED and its process noise ZERO: without it
    //     the harvest would re-level the held column from the same under-determined
    //     epoch the erase does, and the bound would be nominal.
    if (gl_bound && !ctx.carry.gl_hold_amb.empty()) {
        std::set<std::pair<int, std::string>> los_held;
        for (const auto& [key, h] : ctx.carry.gl_hold_amb) {
            ctx.carry.local_carry[key] = h;
            los_held.insert({ std::get<0>(key), std::get<1>(key) });
        }
        for (const auto& [key, h] : ctx.carry.gl_hold_iono)     ctx.carry.iono_carry[key] = h;
        for (const auto& [key, h] : ctx.carry.gl_hold_iono_blk) ctx.carry.iono_blk_carry[key] = h;
        tal.bound_los_epochs += static_cast<long long>(los_held.size());
    }
    if (log && !ctx.carry.gl_act_declare.empty())
        log->debug("Network (gross-LOS, RF-3b): applied {} declaration response(s) as {}; "
                   "{} carried level(s) and {} iono carr(ies) currently held.",
                   ctx.carry.gl_act_declare.size(), gl_bound ? "HOLD" : "erase",
                   ctx.carry.gl_hold_amb.size(),
                   ctx.carry.gl_hold_iono.size() + ctx.carry.gl_hold_iono_blk.size());
}

} // namespace GnssProcessor
