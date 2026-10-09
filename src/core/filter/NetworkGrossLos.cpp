/**
 * @file NetworkGrossLos.cpp
 * @brief Implementation of the gross line-of-sight screen (card GL,
 *        docs/SPEC_event_harness.md §24.18).  NetworkGrossLos.hpp gives the
 *        rationale and the contract.
 *
 * The row walk below uses the same walk and the same family classification as
 * @c networkPostfitSigma0 (`filter/NetworkFrameDiag.cpp`), the pass that measures
 * the residuals this screen acts on.  If the two disagreed about which rows are
 * code, the grade (`network_pf_rows.csv`) and the response would describe
 * different populations.  The differences are that this walk is not gated on a
 * trace being open, and that it acts on what it finds.
 */

#include "filter/NetworkGrossLos.hpp"

#include "app/NetworkRunContext.hpp"      // ctx.knobs / ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"          // config_.getNetworkConfig()
#include "filter/NetworkRowClassify.hpp"  // rowTagAt, isCodeObservationKind, rowObservationCode
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

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkSetupResult.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunTypes.hpp"
#include <cstddef>
#include <memory>

namespace GnssProcessor {

// ---------------------------------------------------------------------------
// Pure rules (unit-tested in tests/test_network_gross_los.cpp)
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
    if (rows.empty()) return d;   // absence is not evidence of health: under_clear stays false
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
    // The bar as a rule (RF-3b B1, requirement F1).  `fixed` (the default) requires at
    // least min_rows rows over k whatever the line of sight has, which cannot be met
    // when it has fewer rows than the constant.  `available` reads the bar as
    // min(min_rows, the LOS's own code rows), so a one-row line of sight is reachable
    // while a three-row one still needs two: the multi-band corroboration the constant
    // exists for is kept wherever the line of sight can supply it.
    const int bar_fixed = std::max(1, min_rows);
    const int bar_avail = std::max(1, std::min(bar_fixed, d.n_rows));
    d.min_rows_eff = min_rows_available ? bar_avail : bar_fixed;
    d.over = d.n_over >= d.min_rows_eff;
    // The population the fixed rule cannot reach (B5, requirement F6), counted under
    // both rules so an arm without B1 still reports what B1 would have reached: the
    // evidence clears the available bar and the line of sight has fewer rows than the
    // constant.
    d.short_over = (d.n_rows < bar_fixed) && (d.n_over >= bar_avail);
    // The release evidence is not `!over`: the band between clear_sigma and k is
    // neither, and a line of sight sitting there keeps whatever disposition it has.
    // That gap is what gives the two bars their hysteresis.
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
            // A guard holds the declaration.  Hold the streak at the requirement:
            // discarding it would lose real evidence, and letting it grow would turn
            // the guard's opening into a burst of simultaneous declarations.  With the
            // streak held, the line of sight declares on the first qualifying epoch
            // after the guard opens and on no earlier one.
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
    // Declared.  Only the release bar matters; any epoch not under it resets the
    // clearing progress.
    if (!v.under_clear) { s.clear_streak = 0; return GrossLosEdge::None; }
    ++s.clear_streak;
    if (s.clear_streak < need_clr) return GrossLosEdge::None;
    s.gross        = false;
    s.clear_streak = 0;
    s.over_streak  = 0;
    return GrossLosEdge::Clear;
}

GrossLosColdEdge networkGrossLosColdStep(GrossLosState& s, const GrossLosVerdict& v,
                                         double bound_m, bool in_hold, bool allow_declare,
                                         double tow) {
    if (!(bound_m > 0.0)) return GrossLosColdEdge::None;
    const bool over = v.n_rows > 0 && v.v_abs_max > bound_m;
    if (s.gross) return (s.cold && over) ? GrossLosColdEdge::Refloat : GrossLosColdEdge::None;
    if (!in_hold || !over) return GrossLosColdEdge::None;
    if (!allow_declare) return GrossLosColdEdge::Held;
    s.gross        = true;
    s.cold         = true;
    s.over_streak  = 0;
    s.clear_streak = 0;
    s.declared_tow = tow;
    ++s.n_declared;
    return GrossLosColdEdge::Declare;
}

// ---------------------------------------------------------------------------
// The actuator's publish half (serial, pre-assembly)
// ---------------------------------------------------------------------------

void networkGrossLosPublishScales(NetworkRunContext& ctx,
                                  std::vector<NetStation>& stations) {
    if (!ctx.knobs.gl_on) return;   // off: the map is never populated, so byte-identical
    const double f = ctx.knobs.gl_deweight;
    // Rebuild from scratch: a released or expired line of sight loses its scale in
    // the same pass that a new declaration gains one, and building fresh maps and
    // swapping them in leaves no window in which a stale key survives.
    std::vector<std::map<std::string, double>> fresh(stations.size());
    for (const auto& [key, st] : ctx.carry.gl_state) {
        if (!st.gross) continue;
        const std::size_t i = static_cast<std::size_t>(key.first);
        if (i >= fresh.size()) continue;
        fresh[i][key.second] = f;
    }
    // A station-level declaration folds all of that station's code rows (RF-3b B4,
    // requirement F4).  The assembly's map is keyed per satellite (the same map the
    // per-LOS response writes), so a station-wide fold is expressed as "every
    // satellite this station was last seen screening".  That set is refreshed each
    // screened epoch, so a satellite that rises while the station is declared is folded
    // from the epoch after it is first screened, and one that sets drops out.
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
// The hold: the bounded decoupling edge (RF-3b B2, standing invariant #7)
// ---------------------------------------------------------------------------
namespace {

/// Freeze one carried entry at its pre-declaration value (RF-3b B2).  The border
/// regression and the joint block are cleared; that is the decoupling, since a
/// regression harvested against a level the screen has declared untrusted would keep
/// injecting that level's relationship to the shared border at full strength.  The
/// value and the variance are kept unchanged.  With q = 0 on the ambiguity block, a
/// prior re-written unchanged after every harvest is a fixed prior with zero
/// process noise.
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
    // The Act half applies the response deferred by this epoch's Decide pass (RF-3b
    // B3).  It is routed through this entry point so the driver has one call shape at
    // both sites.
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
    // --- Whole-satellite stand-down (GL-2, docs/SPEC_event_harness.md §24.19).
    const bool   gl2_on       = ctx.knobs.gl2_on;
    const int    gl2_min_sta  = nc.gross_los_sat_min_stations;
    const bool   gl2_wire     = nc.gross_los_sat_wire_withhold;

    // --- Guard 1: the maturity horizon, counted in emitted epochs.  A tier that
    // fires during ignition measures ignition (invariant #4).  Without the horizon, a
    // 20-epoch cold-start smoke run declared 520 lines of sight; the deweight removed
    // the code information the cold solve needs for the receiver clocks, and the SH
    // health gate withheld 9 of 20 epochs.  During warm-up the screen still screens
    // (every counter and the whole |v|/σ census are live) and only makes no
    // declaration, so the run reports what it would have done.  The RAIM isolated-arc
    // refloat uses the same guard (networkWarmupRampLifted) for the same reason.
    const bool mature = ctx.counters.n_emitted >= static_cast<long long>(warmup_ep);

    // --- Sub-rules of RF-3b.  Each is already ANDed with gl_on at the knob resolve, so an
    // unarmed run takes the unchanged constants and path.
    const bool   mrr_avail    = ctx.knobs.gl_min_rows_available;
    const bool   gl_bound     = ctx.knobs.gl_bound;
    const bool   gl_emit_excl = ctx.knobs.gl_emit_exclude;
    const bool   gl_sta_tier  = ctx.knobs.gl_station_tier;
    // --- The cold-start bound (card ICF response 1); 0 = off, already resolved against
    // the screen and the hold at the knob resolve.  It acts inside the maturity horizon,
    // where the σ-bar declarations are held.
    const double cold_bound   = ctx.knobs.gl_cold_bound_m;
    const bool   cold_window  = cold_bound > 0.0 && !mature;

    auto& tal    = ctx.counters.gross_los;
    auto& gstate = ctx.carry.gl_state;
    ++tal.epochs;
    ++ctx.carry.gl_seq;
    const long long seq = ctx.carry.gl_seq;
    // This epoch's deferred work (RF-3b B3).  Cleared at the start of every screened
    // epoch, so the lists hold only this epoch's edges and emission sets.
    ctx.carry.gl_act_declare.clear();
    ctx.carry.gl_act_release.clear();
    ctx.carry.gl_act_station.clear();
    ctx.carry.gl_act_cold_refloat.clear();
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

    // --- The code rows ----------------------------------------------------------
    // A row is a code row when its contribution tag says so (card NOM step N2b,
    // filter/NetworkRowClassify): the latched code signal's rows and the second code
    // channel's companion rows, with the satellite and the signal the tag names.  The
    // network_pf_rows.csv writer (NetworkFrameDiag) reads the same tags, so the acting
    // screen and the measuring trace agree about which rows are code.  Before the tags
    // the screen inferred it from the shared columns a row touched (a code-bias or clock
    // column, and no phase-bias column or ambiguity touch above 0.5).

    // (station index, satellite) seen with at least one code row this epoch; the
    // absence half of the streak rule works on the complement of this set.
    std::set<std::pair<int, std::string>> seen;
    int n_declared_ep = 0, n_cleared_ep = 0;
    int n_cold_edges_ep = 0;   // cold-start bound: declarations, re-erases, releases, expiries

    // The satellite stand-down (GL-2) needs a network-wide tally before any line of
    // sight is acted on: "the same satellite over the bar at >= N stations in one
    // epoch" is not knowable inside a per-station walk, and a decision taken partway
    // through the walk would depend on the station order.  The pass is therefore
    // split: assess every station, tally per satellite, then act.  The store holds
    // about 61 x 10 verdicts.
    struct StationPass {
        std::size_t i{0};
        std::map<std::string, GrossLosVerdict> verdicts;
        bool sta_standdown{false};
        bool cold_standdown{false};   ///< The cold station guard holds cold declarations here (card ICF).
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

        // The scale map the assembly read for this epoch.  This seam republishes it
        // only at its own end, so the map here is the one that was folded.  The
        // statistic undoes the fold with it; see the row build below.
        const std::map<std::string, double>& gl_scale = stations[i].gl_code_sigma_scale;

        std::map<std::string, std::vector<GrossLosSample>> by_sat;
        for (Eigen::Index row = 0; row < v.size(); ++row) {
            // Only the code observations are screened: the phase side has its own tiers,
            // the SH tie is a documented 3-sigma artifact, and the pins, prior rows and
            // coupling rows are not observations.
            const RowTag& tag = rowTagAt(R, row);
            if (!isCodeObservationKind(tag.kind) || !tag.sat.valid()) continue;
            const std::string row_sat = tag.sat.str();
            const std::string row_sig = rowObservationCode(tag);

            double gl_scale_here = 1.0;
            if (!gl_scale.empty()) {
                const auto sc = gl_scale.find(row_sat);
                if (sc != gl_scale.end()) gl_scale_here = sc->second;
            }
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            // The statistic must not be reduced by the response it drives.  A
            // declared line of sight's code rows were folded this epoch at σ × f,
            // so |v|·√w reads f times smaller than the same residual at nominal
            // weight.  At f = 100 a 144 m residual declared at 125 σ reads 1.3 σ one
            // epoch later, under the 3 σ release bar, and the line of sight then
            // alternates between declare and clear every other epoch.  Undo the fold
            // exactly: `gl_code_sigma_scale` is the map the assembly read for this
            // epoch (this seam republishes it only at its end), so multiplying by it
            // restores the row's nominal σ, and both bars measure the data rather
            // than the response.
            const double r = networkGrossLosUndoDeweight(
                std::abs(v(row)) * std::sqrt(std::max(0.0, w)), gl_scale_here);
            if (!std::isfinite(r)) continue;
            // The requirement F2 alarm (RF-3b B5, requirement F6): the largest |v| on a
            // row that is actually folded.  A folded row's residual should stay a
            // measure of the fault; a growing one (hundreds of metres) is how an
            // unbounded decoupling edge appears from outside.  Counted whether or not
            // network.gross_los_bound is armed, so the two arms are comparable.
            if (gl_scale_here > 1.0) {
                const double av_f = std::abs(v(row));
                if (std::isfinite(av_f) && av_f > tal.folded_v_abs_max_m)
                    tal.folded_v_abs_max_m = av_f;
            }
            by_sat[row_sat].push_back(GrossLosSample{ row_sig, r, v(row) });
        }
        if (by_sat.empty()) continue;
        ++tal.stations_screened;

        // Assess every line of sight of the station before acting on any of them:
        // guard 2 needs the station's whole over-bar fraction, and a decision taken
        // partway through the alphabetical walk would depend on the satellite order.
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
            // Population of B1 (RF-3b B5), counted under both rules (the verdict fills
            // it either way): LOS-epochs whose evidence clears the available bar while
            // the line of sight has fewer code rows than the configured constant.  Under
            // the fixed rule, this is what the screen cannot reach.
            if (d.short_over) ++tal.los_over_short;
            ++n_los_here;
            verdicts.emplace(sat, d);
        }
        // --- Guard 2: the whole-station stand-down ---------------------------
        // A gross line of sight is isolated by definition.  When most of a station's
        // lines of sight are over the bar, the fault is the station's (for example a
        // diverged ZWD state, with every satellite of the station tens of metres off),
        // and deweighting every code row of that station removes the information that
        // determines its clock and its troposphere.  This screen cannot repair a
        // station, so it stands down and reports that in its own event.  On a field
        // run with such a collapse, the per-station over-bar fraction had p90 = 0.00 in
        // the quiet hours and 0.11-0.12 in the two worst hours, with a maximum of
        // 0.88-1.00, so the 0.5 default cap fires on the collapse and not on an
        // isolated line of sight.  Clears are not blocked (see the step's contract): a
        // guard that froze the release edge would leave the response with no exit.
        // Evaluated only once the maturity horizon has opened: before that nothing can
        // be declared, so the stand-down has nothing to guard, and emitting it through
        // a cold start would fill the event stream with the ignition transient rather
        // than station faults.
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
        // --- The station tier (RF-3b B4, requirement F4) ---------------------
        // The guard above holds only this epoch's declarations; at the next epoch, the
        // same lines of sight can declare one by one on the same evidence, turning one
        // station fault into N line-of-sight faults.  With the tier armed, the guard's
        // condition becomes a station-level verdict with its own two edges.  At the
        // declaration, every one of the station's code rows folds at the deweight and
        // its states receive the per-LOS response (erase, or hold under
        // network.gross_los_bound).  The station is released when its over-bar
        // fraction sits at or under the cap for gross_los_clear_epochs consecutive
        // screened epochs.  The release bar is the cap and not gross_los_clear_sigma:
        // the station-level statistic is the fraction, so its hysteresis is the streak.
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

        // --- The cold station guard (card ICF response 1) ---------------------------
        // The whole-station rule above, read on the cold-start bound: when most of a
        // station's lines of sight are over the bound in one epoch, the fault is the
        // station's, and folding all of its code rows would remove what determines its
        // clock.  Evaluated inside the horizon only, where the cold path declares.
        bool cold_standdown = false;
        if (cold_window && max_sta_frac < 1.0 && n_los_here > 0) {
            int n_cold_over_here = 0;
            for (const auto& [sat, d] : verdicts) {
                (void)sat;
                if (d.n_rows > 0 && d.v_abs_max > cold_bound) ++n_cold_over_here;
            }
            cold_standdown = static_cast<double>(n_cold_over_here)
                           > max_sta_frac * static_cast<double>(n_los_here);
            if (cold_standdown) {
                ++tal.cold_standdowns;
                emitEvent("gross_los_cold_standdown", "", stations[i].name,
                          static_cast<double>(n_cold_over_here)
                              / static_cast<double>(n_los_here),
                          max_sta_frac, "standdown",
                          fmt::format("n_over_bound={};n_los={};bound_m={:g};"
                                      "reason=whole_station_not_one_los",
                                      n_cold_over_here, n_los_here, cold_bound));
            }
        }

        if (gl2_on)
            for (const auto& [sat, d] : verdicts)
                if (d.over) ++sat_over_stations[sat];
        passes.push_back(StationPass{ i, std::move(verdicts), sta_standdown, cold_standdown });
    }

    // --- The whole-satellite stand-down (GL-2, §24.19) --------------------------
    // A satellite over the bar at several stations in the same epoch is not several
    // gross lines of sight; it is a satellite fault (its clock, its orbit or its code
    // bias).  In the case behind §24.19 (measured on the §24.18 arm), the per-LOS
    // screen declared ten lines of sight of one satellite at ten stations on four
    // continents in one epoch; deweighting all of them removed the satellite from the
    // code solution network-wide, and the held-out consumer error reached 56 m and
    // decayed over an
    // hour.
    //
    // This is the whole-station rule applied along the other axis, and it is the
    // population post-fit RAIM's cross-station consensus exists to route
    // (`network.raim_min_stations`, default 3: "a satellite so flagged at >= 3
    // stations is refloated network-wide").  GL-2 therefore adds no response of its
    // own; it routes the satellite to the two mechanisms the producer already has
    // for a consensus fault:
    //   * the per-satellite retro disc bump, by queueing the satellite into
    //     `ctx.carry.ura_retro_sats`, the queue `NetworkSolPrep` drains into
    //     `emitter.markSatDiscontinuity`, with that seam's mark-==-land semantics
    //     (a satellite absent from an emission stays pending) and its RETRO event.
    //     NetworkSolPrep drains the queue only when one of its own arming terms is
    //     on (the B1 retro-bump, reference-path slip marks, the datum-station slip
    //     re-reference, the held-basis arm, or network.gross_los_emit_exclude).
    //     GL-2 adds no term there, so with none of them armed the queued satellites
    //     stay pending and no mark lands;
    //   * the per-satellite wire withhold, by queueing it into
    //     `ctx.carry.gl2_wire_pending`, which `NetworkSolPrep` prunes out of
    //     `sol.sats` the way the RT0-5 runaway quarantine and the RTA-10
    //     per-constellation carve-out prune theirs.  Never an epoch withhold: the
    //     epoch still emits every other satellite (adapt-and-emit).
    // In the Full phase both land one epoch late, the lag every post-fit tier here
    // has (the emit seams run before SB_RAIM), which is why the consensus path is
    // edge-triggered and pending-aware.  In the Decide phase
    // (network.gross_los_emit_exclude) this pass runs before NetworkSolPrep, so both
    // land at the epoch that stood the satellite down.
    std::set<std::string> sat_down_now;
    if (gl2_on && mature)
        for (const auto& [sat, n] : sat_over_stations)
            if (n >= gl2_min_sta) sat_down_now.insert(sat);

    if (gl2_on) {
        // The decoupling edge: a satellite that was not stood down and now is.
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
        // The re-coupling edge: a satellite that was stood down and is not any more.
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
        // Route the currently stood-down set to the producer's consensus-fault
        // response.  The wire set is rebuilt every epoch, so a released satellite stops
        // being withheld in the same pass a newly faulted one starts.
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

    // --- Act ------------------------------------------------------------------
    for (auto& pass : passes) {
        const std::size_t i = pass.i;
        const bool sta_standdown = pass.sta_standdown;
        for (const auto& [sat, d] : pass.verdicts) {
            const auto key = std::make_pair(static_cast<int>(i), sat);
            seen.insert(key);
            GrossLosState& st = gstate[key];
            st.last_seen_s = now_s;
            // A stood-down satellite (GL-2) takes its lines of sight with it.  An
            // already-declared one is released here: the deweight lifts, because the
            // fault is not this line of sight's, and keeping it decoupled would keep
            // removing code the solution needs (standing invariant #7's re-coupling
            // edge, at the satellite's scale).  The release is counted
            // (sat_los_released) but does not emit the per-LOS clear event, which
            // states that the line of sight's data recovered; here they did not.
            const bool sat_down = gl2_on && ctx.carry.gl2_sat_down.count(sat) > 0;
            if (sat_down && st.gross) {
                st.gross = false;
                st.over_streak = 0;
                st.clear_streak = 0;
                ++tal.sat_los_released;
                // The hold is part of the response, so it lifts with the response
                // (RF-3b B2): a released line of sight must not stay frozen.
                ctx.carry.gl_act_release.push_back({ static_cast<int>(i), sat });
                // A cold declaration (card ICF) reconnects on a fresh ambiguity at every
                // exit, this one included: its carry was harvested while the fault was in
                // its rows.
                if (st.cold) {
                    st.cold = false;
                    ++tal.cold_released;
                    ++n_cold_edges_ep;
                    ctx.carry.gl_act_cold_refloat.push_back({ static_cast<int>(i), sat });
                    emitEvent("gross_los_cold_release", sat, stations[i].name, d.v_abs_max,
                              cold_bound, "release",
                              fmt::format("n_rows={};declared_tow={:.1f};episode={};"
                                          "reason=sat_standdown;reseed=erase",
                                          d.n_rows, st.declared_tow, st.n_declared));
                }
            }
            // --- The cold-start bound (card ICF response 1) --------------------------
            // Inside the horizon, a line of sight far beyond any plausible residual is
            // declared at once with the screen's own response (the deferred erase below
            // and the code fold the publish half applies).  A standing cold declaration
            // whose bound is exceeded again has its carried ambiguity erased again, so a
            // re-stashed ambiguity cannot make the faulty line of sight redundant through
            // its phase rows.  Off (bound 0) the step returns None and nothing here runs.
            if (cold_bound > 0.0) {
                if (cold_window) {
                    ++tal.cold_los_screened;
                    const bool over_b = d.n_rows > 0 && d.v_abs_max > cold_bound;
                    if (over_b) {
                        ++tal.cold_los_over;
                        tal.cold_v_over_max_m = std::max(tal.cold_v_over_max_m, d.v_abs_max);
                    } else if (!st.gross) {
                        tal.cold_v_under_max_m = std::max(tal.cold_v_under_max_m, d.v_abs_max);
                    }
                }
                const GrossLosColdEdge cedge = networkGrossLosColdStep(
                    st, d, cold_bound, cold_window, !pass.cold_standdown, se.time.tow);
                if (cedge == GrossLosColdEdge::Held) {
                    ++tal.cold_held_station;
                } else if (cedge == GrossLosColdEdge::Declare) {
                    ++tal.cold_declared;
                    ++tal.cold_declared_by_los[stations[i].name + ":" + sat];
                    ++n_cold_edges_ep;
                    // The screen's own erase, deferred to the act half (it must run after
                    // the carry harvest) through the cold list, whose pass also sweeps a
                    // carried ambiguity of a signal absent this epoch; the publish below
                    // folds the code rows.
                    ctx.carry.gl_act_cold_refloat.push_back({ static_cast<int>(i), sat });
                    emitEvent("gross_los_cold_declare", sat, stations[i].name, d.v_abs_max,
                              cold_bound, "declare",
                              fmt::format("n_rows={};r_max={:.1f};emitted_epochs={};"
                                          "horizon={};episode={};deweight_sigma_x={:g};"
                                          "response=erase",
                                          d.n_rows, d.r_max, ctx.counters.n_emitted,
                                          warmup_ep, st.n_declared, ctx.knobs.gl_deweight));
                    continue;   // the σ-bar step has nothing to add on the declaring epoch
                } else if (cedge == GrossLosColdEdge::Refloat) {
                    ++tal.cold_refloats;
                    if (!cold_window) ++tal.cold_refloats_after_hold;
                    ++n_cold_edges_ep;
                    ctx.carry.gl_act_cold_refloat.push_back({ static_cast<int>(i), sat });
                    emitEvent("gross_los_cold_refloat", sat, stations[i].name, d.v_abs_max,
                              cold_bound, "refloat",
                              fmt::format("n_rows={};declared_tow={:.1f};episode={};"
                                          "in_hold={}",
                                          d.n_rows, st.declared_tow, st.n_declared,
                                          cold_window ? 1 : 0));
                }
            }
            // Origin of the per-episode latency (RF-3b B5).  Stamped when an over-bar
            // streak opens and cleared when it ends, so a declaration can state how
            // long the evidence stood, not only when the response acted.
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
            // A station-level declaration replaces the per-LOS ones (RF-3b B4), so it
            // forbids new per-LOS declarations at that station for as long as it
            // stands, as the guard does for one epoch.  Releases stay allowed (the
            // step's own contract).
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
                // --- The response (decoupling edge) --------------------------
                // Deferred here and applied by @ref networkGrossLosAct (RF-3b B3).  The
                // Full phase calls it at the end of this same pass (byte-identical to
                // the unsplit screen); the split phases call it after the carry
                // harvest.  Either way it runs after the harvest, because a
                // local_carry entry erased or frozen before the harvest would be
                // re-stashed.
                ctx.carry.gl_act_declare.push_back({ static_cast<int>(i), sat });
                // Per-episode latency (RF-3b B5), in over-bar screened epochs, from the
                // first epoch over the bar to the declaration.
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
            } else if (edge == GrossLosEdge::Clear && st.cold) {
                // The re-coupling edge of a cold declaration (card ICF response 1): the
                // screen's release test passed, and the carried ambiguity is erased once
                // more, because it was harvested while the fault was in the line of
                // sight's rows; the line of sight reconnects on a fresh ambiguity.
                st.cold = false;
                ++tal.cold_released;
                ++n_cold_edges_ep;
                ctx.carry.gl_act_release.push_back({ static_cast<int>(i), sat });
                ctx.carry.gl_act_cold_refloat.push_back({ static_cast<int>(i), sat });
                emitEvent("gross_los_cold_release", sat, stations[i].name, d.r_max, clear_sigma,
                          "release",
                          fmt::format("n_rows={};v_abs_max_m={:.3f};declared_tow={:.1f};"
                                      "episode={};reseed=erase",
                                      d.n_rows, d.v_abs_max, st.declared_tow, st.n_declared));
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

    // --- Absence: the streak reset and the absent-satellite edge ----------------
    // Standing invariant #1: a declared line of sight that stops being observed does
    // not keep its response indefinitely.  The next sighting is a new arc with a new
    // ambiguity, and the old evidence does not apply to it.  Past
    // gross_los_max_gap_s, every unseen entry is erased; a declared one first
    // expires with its own event.
    for (auto it = gstate.begin(); it != gstate.end(); ) {
        const bool here = seen.count(it->first) != 0;
        if (here) { ++it; continue; }
        it->second.over_streak  = 0;   // absence is not evidence either way
        it->second.clear_streak = 0;
        const double gap = now_s - it->second.last_seen_s;
        if (gap <= max_gap_s) { ++it; continue; }
        if (it->second.gross && it->second.cold) {
            // The cold path's absent-satellite exit (card ICF response 1), with the same
            // re-seed as its release: whatever carry the line of sight still holds was
            // harvested while the fault was in its rows.
            ++tal.cold_expired;
            ++n_cold_edges_ep;
            const std::size_t si = static_cast<std::size_t>(it->first.first);
            emitEvent("gross_los_cold_expire", it->first.second,
                      si < stations.size() ? stations[si].name : std::string(),
                      gap, max_gap_s, "expire",
                      fmt::format("unseen_s={:.1f};declared_tow={:.1f};episode={};reseed=erase",
                                  gap, it->second.declared_tow, it->second.n_declared));
            ctx.carry.gl_act_release.push_back({ it->first.first, it->first.second });
            ctx.carry.gl_act_cold_refloat.push_back({ it->first.first, it->first.second });
        } else if (it->second.gross) {
            ++tal.expired;
            const std::size_t si = static_cast<std::size_t>(it->first.first);
            emitEvent("gross_los_expire", it->first.second,
                      si < stations.size() ? stations[si].name : std::string(),
                      gap, max_gap_s, "expire",
                      fmt::format("unseen_s={:.1f};declared_tow={:.1f};episode={}",
                                  gap, it->second.declared_tow, it->second.n_declared));
            // The expiry is the other exit from a declaration, so it drops the hold too
            // (RF-3b B2); otherwise a frozen prior would stay on a line of sight the
            // screen has stopped tracking (standing invariant #1's absent-satellite edge).
            ctx.carry.gl_act_release.push_back({ it->first.first, it->first.second });
        }
        it = gstate.erase(it);
    }

    long long now_gross = 0;
    for (const auto& kv : gstate) if (kv.second.gross) ++now_gross;
    tal.currently_gross = now_gross;
    if (now_gross > tal.max_concurrent) tal.max_concurrent = now_gross;

    // --- The emission's contributor set (RF-3b B3, requirement F3) ----------------
    // In the Full phase the screen runs after the emit seams, so a declaration first
    // affects the wire one epoch later, and the rows that triggered it have already
    // been emitted.  With the seam split, this pass runs before the emission, and a
    // declared line of sight (or every line of sight of a declared station) leaves
    // its satellite's contributor set at every screened epoch the declaration
    // stands, starting with the epoch it declares.
    //
    // Scope limit: the emitted clock and code bias are joint-solve shared states
    // (NetworkEstimator::toSolutionEpoch copies x_s(c) straight into clock_c0_m /
    // code_bias_m), so there is no per-contributor decomposition to subtract and no
    // aggregate to re-average.  Changing the emitted value without the line of sight
    // would take a second network solve of the epoch, which this knob does not do.
    // Instead, a satellite whose remaining contributors fall below
    // network.gross_los_sat_min_stations has its records withheld; otherwise its
    // records are emitted with the producer's existing per-satellite discontinuity
    // mark, so the consumer is told the contributor set changed instead of seeing an
    // unsignalled step.
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
    // behind.  Serial and immediately after the decision, so the next epoch's
    // parallel produce reads a settled map (the phase_sigma_scale contract).
    networkGrossLosPublishScales(ctx, stations);

    // The Full phase applies the deferred response here (RF-3b B3), at the
    // post-harvest site, so its ordering matches the unsplit screen.  When split, the
    // Act half runs from its own call after the harvest.
    if (phase == GrossLosPhase::Full)
        networkGrossLosAct(ctx, log, stations, asm_res);

    if (n_declared_ep > 0 || n_cleared_ep > 0)
        log->info("Network (gross-LOS screen) TOW {:.1f}: {} declared, {} cleared, "
                  "{} currently gross (run-max standardized code residual so far {:.1f} sigma)",
                  se.time.tow, n_declared_ep, n_cleared_ep, now_gross, tal.r_max);
    if (n_cold_edges_ep > 0)
        log->info("Network (gross-LOS, card ICF cold bound {:g} m) TOW {:.1f}: {} cold edge(s) "
                  "or re-erase(s) this epoch; run totals {} declared, {} re-erased, {} released, "
                  "{} expired.",
                  cold_bound, se.time.tow, n_cold_edges_ep, tal.cold_declared,
                  tal.cold_refloats, tal.cold_released, tal.cold_expired);
}

// ---------------------------------------------------------------------------
// The deferred response: the post-harvest half (RF-3b B3)
// ---------------------------------------------------------------------------

void networkGrossLosAct(NetworkRunContext& ctx,
                        const std::shared_ptr<spdlog::logger>& log,
                        std::vector<NetStation>& stations,
                        const std::vector<StationAssembly>& asm_res) {
    if (!ctx.knobs.gl_on) return;
    auto& tal      = ctx.counters.gross_los;
    const bool gl_bound = ctx.knobs.gl_bound;

    // --- The per-LOS response.  The two forms are exclusive.  With
    //     network.gross_los_bound off it is the erase (a full rebirth of the LOS's
    //     states); with it on it is the hold, which bounds what the freed states can
    //     accumulate (standing invariant #7's decoupling edge).  The erase has a
    //     measured cost: with only a folded code row left to determine them, the freed
    //     states re-level from an under-determined epoch, the folded row's residual
    //     can grow to hundreds of metres, and the clear test then never passes, so
    //     such a declaration ends by expiry rather than release (2 of 7 in the
    //     RF-3 measurement).
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
                    prmRespond(ctx.counters.prm_resp,
                               ResponseSite::AmbGrossHold);   // card PRM phase 3a
                }
                // No pending-reseed under the hold: the state is pinned, not
                // refloated.  Marking it would give the next assembly a free column,
                // which is what the hold exists to prevent.
            } else {
                if (ctx.carry.local_carry.erase(lk)) {
                    ++ctx.counters.n_cr_gross_los;
                    prmRespond(ctx.counters.prm_resp,
                               ResponseSite::AmbGrossErase);   // card PRM phase 3a
                }
                // Under carry_invariant this mark is bookkeeping; on the
                // phase-leveling vehicle it is the actuator (the same asymmetry the
                // RAIM erase documents).
                if (stations[i].sm) stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
            }
            // The gross-LOS writer of the arc-ledger reason buffer (card SID phase 4a,
            // S1).  The screen's published action is a code-row deweight (a cap, not a
            // drop: the rows still fold), so the arc is recorded with the `deweight`
            // bit, which kArcReasonGapMask excludes from every gap mask.  The `gross`
            // bit records a removal of carried state, so it is written on the erase
            // path only; a held arc keeps its state, and a gap mask must not read it
            // as gone.  This runs inside SB_RAIM, before the roster's audit pass folds
            // the buffer (arcReasonBeginEpoch opens it at the roster build), so both
            // bits reach the fold of their own epoch and need no kArcReasonLateMask
            // carry.
            arcReasonRecord(ctx.reasons,
                            ArcLedger::Key{ si, pa.sat, pa.signal }, kArcReasonDeweight);
            if (!gl_bound)
                arcReasonRecord(ctx.reasons,
                                ArcLedger::Key{ si, pa.sat, pa.signal }, kArcReasonGross);
            ++n_amb;
        }
        tal.amb_refloated += n_amb;
        // The LOS's slant iono was estimated jointly with a level this screen has just
        // declared untrusted, and the code residual the screen fired on is the channel
        // that feeds it, so the iono carry is not trusted either.  Under the hold it is
        // frozen for the same reason as the level: a re-drawn iono would have to be
        // determined by the folded code row, which cannot do it.
        const std::pair<int, std::string> ik{ si, sat };
        if (gl_bound) {
            const auto it1 = ctx.carry.iono_carry.find(ik);
            if (it1 != ctx.carry.iono_carry.end()) {
                const LocalCarry h = glFreeze(it1->second);
                ctx.carry.gl_hold_iono[ik] = h;
                it1->second = h;
                ++tal.bound_iono_held;
                prmRespond(ctx.counters.prm_resp,
                           ResponseSite::IonoGrossHold);   // card PRM phase 3a
            }
            const auto it2 = ctx.carry.iono_blk_carry.find(ik);
            if (it2 != ctx.carry.iono_blk_carry.end()) {
                const LocalCarry h = glFreeze(it2->second);
                ctx.carry.gl_hold_iono_blk[ik] = h;
                it2->second = h;
                ++tal.bound_iono_held;
                prmRespond(ctx.counters.prm_resp,
                           ResponseSite::IonoGrossHold);   // card PRM phase 3a
            }
        } else {
            // Card PRM phase 3a: each erase that removed an entry is counted.
            if (ctx.carry.iono_carry.erase(ik)) {
                ++tal.iono_erased;
                prmRespond(ctx.counters.prm_resp, ResponseSite::IonoGrossErase);
            }
            if (ctx.carry.iono_blk_carry.erase(ik)) {
                ++tal.iono_erased;
                prmRespond(ctx.counters.prm_resp, ResponseSite::IonoGrossErase);
            }
            // Card LGM: a gross line of sight's phase-colour entry re-starts from its
            // birth prior as well (the gross phase rows wrote into it).
            if (ctx.carry.pc_carry.erase(ik)) {
                ++ctx.counters.n_pc_erase_gross;
                prmRespond(ctx.counters.prm_resp, ResponseSite::PcGrossErase);
            }
        }
    };

    for (const auto& [si, sat] : ctx.carry.gl_act_declare) respond(si, sat);
    // A station declaration applies the same response to every satellite the station
    // was last seen screening, as one verdict rather than N sequential ones (RF-3b B4).
    // A station release queues a release for each of those satellites.
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
    // --- The cold-start bound's erase (card ICF response 1): at the declaration, at each
    //     epoch the bound is exceeded again, and as the re-seed at the release or the
    //     expiry.  The screen's own erase (the bound is resolved off under the hold, so
    //     respond is the erase here), followed by a sweep of any other carried ambiguity
    //     of the line of sight that is not in this epoch's assembly (a signal absent this
    //     epoch, or a line of sight that has expired), so no carried level harvested
    //     while the fault was in the rows survives the response.
    for (const auto& [si, sat] : ctx.carry.gl_act_cold_refloat) {
        const std::size_t n_before = ctx.carry.local_carry.size();
        respond(si, sat);
        for (auto it = ctx.carry.local_carry.begin(); it != ctx.carry.local_carry.end(); ) {
            if (std::get<0>(it->first) == si && std::get<1>(it->first) == sat)
                it = ctx.carry.local_carry.erase(it);
            else
                ++it;
        }
        tal.cold_amb_erased +=
            static_cast<long long>(n_before - ctx.carry.local_carry.size());
    }
    // --- The re-coupling edge: a release or an expiry drops the hold, and the states
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
    // --- Re-apply the hold over whatever the harvest re-stashed (RF-3b B2).  This
    //     makes the prior fixed with zero process noise; without it, the harvest would
    //     re-level the held column from the same under-determined epoch as the erase,
    //     and the bound would have no effect.  The held iono entries are re-applied
    //     only in the same pass, i.e. only while at least one carried level is held.
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

// ---------------------------------------------------------------------------
// Run-end readout of the cold-start bound (card ICF response 1)
// ---------------------------------------------------------------------------

void networkGrossLosColdBoundRunEnd(const NetworkRunContext& ctx,
                                    const GnssConfig& config_,
                                    const std::shared_ptr<spdlog::logger>& log) {
    const double configured = config_.networkGrossLosColdBoundM();
    if (!log || !(configured > 0.0)) return;   // off: nothing to report
    const NetworkConfig& nc = config_.getNetworkConfig();
    const double bound = ctx.knobs.gl_cold_bound_m;
    if (!(bound > 0.0)) {
        log->warn("Network (gross-LOS, card ICF cold bound): INERT.  "
                  "network.gross_los_cold_bound_m = {:g} m was set but resolved off "
                  "(network.gross_los_screen {}, network.gross_los_bound {}); the run is the "
                  "unarmed run.",
                  configured, nc.gross_los_screen ? "on" : "off",
                  nc.gross_los_bound ? "on" : "off");
        return;
    }
    const auto& g = ctx.counters.gross_los;
    long long standing = 0;
    for (const auto& kv : ctx.carry.gl_state)
        if (kv.second.gross && kv.second.cold) ++standing;

    log->info("Network (gross-LOS, card ICF cold bound {:g} m): {} LOS-epoch(s) screened inside "
              "the maturity horizon ({} emitted epochs), {} over the bound.  Largest post-fit "
              "code |v| over the bound {:.2f} m; largest under it on a line of sight not "
              "declared {:.2f} m, the bound's margin on this run (x{:.2f}).",
              bound, g.cold_los_screened, nc.gross_los_warmup_epochs, g.cold_los_over,
              g.cold_v_over_max_m, g.cold_v_under_max_m,
              g.cold_v_under_max_m > 0.0 ? bound / g.cold_v_under_max_m : 0.0);
    log->info("Network (gross-LOS, card ICF cold bound): {} declaration(s) (decoupling edges), "
              "{} release(s) and {} expiry(ies) (re-coupling edges, each re-seeding the "
              "carried ambiguity), {} still declared at run end; {} re-erase(s) of a standing "
              "declaration whose bound was exceeded again ({} after the horizon opened); {} "
              "carried ambiguity entr(ies) removed by these erases.  The cold station guard "
              "fired on {} station-epoch(s) and held {} LOS-epoch(s) over the bound.",
              g.cold_declared, g.cold_released, g.cold_expired, standing, g.cold_refloats,
              g.cold_refloats_after_hold, g.cold_amb_erased, g.cold_standdowns,
              g.cold_held_station);
    if (g.cold_declared == 0) {
        const char* why =
            (g.cold_los_screened == 0)
                ? "the seam never screened a line of sight inside the horizon (no code rows "
                  "reached it, or the horizon is 0)"
            : (g.cold_los_over == 0)
                ? "ZERO OPPORTUNITY: no line of sight reached the bound inside the horizon; a "
                  "clean window, not a demonstration"
                : "every line of sight over the bound was held by the cold station guard";
        log->info("Network (gross-LOS, card ICF cold bound): INERT on this run: {}.", why);
    }
    if (!g.cold_declared_by_los.empty()) {
        std::string line;
        for (const auto& [los, n] : g.cold_declared_by_los)
            line += fmt::format("{}{}={}", line.empty() ? "" : " ", los, n);
        log->info("Network (gross-LOS, card ICF cold bound): declarations by line of sight: {}",
                  line);
    }
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the gross line-of-sight screen.
int networkGrossLosRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const std::vector<NetStation>& stations = v.setup.stations;

    // --- Gross line-of-sight screen run-end telemetry ---------------------------
    // (card GL, docs/SPEC_event_harness.md §24.18.)  These counters are the ON-path
    // telemetry (invariant #4).  The denominators come first so that four outcomes that
    // all look like "the screen found nothing" stay distinguishable: the seam never
    // ran, no code row reached it, nothing cleared the bar, or the evidence never
    // persisted.  The per-station and per-satellite declaration tables come last; they
    // are the identities the grade needs (invariant #7: an edge without an identity is
    // not an event).
    if (ctx.knobs.gl_on) {
        // The fold runs inside the parallel produce phase, so its row count is kept per
        // station (as si_rows_demoted is); it is summed here, once, before the block
        // reads it.
        auto gross_los = ctx.counters.gross_los;   // a local copy: the folded-row sum below is this report's own
        gross_los.rows_deweighted = 0;
        for (const auto& st : stations) gross_los.rows_deweighted += st.gl_rows_deweighted;
        const auto& g = gross_los;
        log->info("Network (gross-LOS screen): {} seam pass(es), {} station-epoch(s) with "
                  "code rows, {} LOS-epoch(s) screened over {} code row(s).",
                  g.epochs, g.stations_screened, g.los_screened, g.rows_screened);
        if (g.epochs > 0 && g.rows_screened == 0)
            log->warn("Network (gross-LOS screen): DEAD WIRING — the seam ran {} time(s) and "
                      "NO code row ever reached it.  The classification is the same one "
                      "networkPostfitSigma0 uses (a DATA row that is not phase and touches a "
                      "CB: or CLK: shared column); an empty census means the posture "
                      "registers neither, so check the shared-column labels before reading "
                      "this run as clean.",
                      g.epochs);
        log->info("Network (gross-LOS screen): {} LOS-epoch(s) over the {:.1f}σ bar on ≥ {} "
                  "row(s); worst standardized code residual {:.1f}σ, worst |v| {:.2f} m.",
                  g.los_over, config_.getNetworkConfig().gross_los_k,
                  config_.getNetworkConfig().gross_los_min_rows, g.r_max, g.v_abs_max_m);
        if (g.los_screened > 0 && g.los_over == 0)
            log->info("Network (gross-LOS screen): ZERO OPPORTUNITY — it screened real code "
                      "residuals and nothing cleared the bar.  A clean window, not a "
                      "demonstration: the screen's power is unproven until it is graded "
                      "against a window that carries a known gross line of sight.");
        else if (g.los_over > 0 && g.declared == 0)
            log->warn("Network (gross-LOS screen): {} LOS-epoch(s) cleared the bar but NOTHING "
                      "was declared — the evidence never persisted {} consecutive epoch(s).  "
                      "That is the persistence gate doing its job on heavy tails, or it is "
                      "the gate starving a real fault that flickers; the |v|/σ distribution "
                      "in network_pf_rows.csv is what separates the two.",
                      g.los_over, config_.getNetworkConfig().gross_los_epochs);
        log->info("Network (gross-LOS screen) GUARDS: {} LOS-epoch(s) held by the MATURITY "
                  "horizon ({} emitted epochs) and {} by the WHOLE-STATION stand-down, which "
                  "fired on {} station-epoch(s).  A held LOS-epoch is a COMPLETE declaration a "
                  "guard refused — never a silent one.",
                  g.suppressed_warmup, config_.getNetworkConfig().gross_los_warmup_epochs,
                  g.suppressed_station, g.standdowns);
        log->info("Network (gross-LOS screen): {} declaration(s), {} release(s), {} "
                  "expiry(ies) on the absent-satellite edge; {} currently gross at run end, "
                  "{} the largest simultaneous set.",
                  g.declared, g.cleared, g.expired, g.currently_gross, g.max_concurrent);
        log->info("Network (gross-LOS screen): the RESPONSE moved {} carried level entry(ies) "
                  "(erase + pending-reseed), {} per-LOS iono carry entry(ies), and folded {} "
                  "CODE row(s) at σ × {:g}.  Nothing here withheld an epoch — the screen is "
                  "adapt-and-emit by construction.",
                  g.amb_refloated, g.iono_erased, g.rows_deweighted,
                  config_.getNetworkConfig().gross_los_deweight);
        log->info("Network (gross-LOS screen): of those, {} were leveling-carry entries erased "
                  "(retirement census family [C], the same one-solve refloat the T4 attribution "
                  "erase performs — the arc stays tracked and the next harvest re-stashes it).",
                  ctx.counters.n_cr_gross_los);
        if (ctx.knobs.amb_float_kf)
            log->info("Network (gross-LOS screen): on the float vehicle the T4 attribution "
                      "drops a carried arc instead of erasing it, so the gross screen's erase is "
                      "the stronger response of the two (card PRM phase 3a).");
        // --- Per-rule readout (docs/refound/RF-3_code_row_qc_2026-09-22.md §4 B5,
        //     requirement F6; RF-3b).  Every line prints in both states of the sub-knob
        //     it reports on.  The populations a rule targets (B1's short-row count over
        //     the bar, B2's largest folded |v|, releases and expiries) are counted in both
        //     states, so an arm that leaves a rule off still states the population it
        //     would have reached (invariant #4).  The counts of a rule's own actions (B2
        //     holds, B3 exclusions, B4 station declarations) move only with it on.
        {
            const NetworkConfig& gnc3 = config_.getNetworkConfig();
            const bool mrr_av = gnc3.gross_los_min_rows_rule
                                    == NetworkGrossLosMinRowsRule::Available;
            log->info("Network (gross-LOS, RF-3b B1 min_rows rule = {}): {} LOS-epoch(s) "
                      "cleared the AVAILABLE bar while the line of sight had FEWER than {} "
                      "code rows.  That population is UNREACHABLE under the fixed rule and "
                      "is exactly where four of the six largest episodes of the RF-3 read "
                      "live (a line of sight whose station has lost every signal but one "
                      "band-1 code).  {} of the run's {} declaration(s) were taken at a "
                      "reduced bar.",
                      mrr_av ? "available" : "fixed", g.los_over_short,
                      gnc3.gross_los_min_rows,
                      mrr_av ? "Some" : "None", g.declared);
            log->log((g.expired > 0) ? spdlog::level::warn : spdlog::level::info,
                     "Network (gross-LOS, RF-3b B2 bound = {}): {} release(s) against {} "
                     "expiry(ies) — RELEASE is the intended exit and EXPIRY the exception, "
                     "because an expiry means the clear test could never fire.  The largest "
                     "|v| any FOLDED row reached was {:.3f} m (the F2 alarm: 445.48 m on "
                     "runs/_rap_rf3_gl1 with the bound off).  The hold froze {} carried "
                     "level(s) and {} iono carr(ies) and was re-applied over {} "
                     "LOS-epoch(s).",
                     gnc3.gross_los_bound ? "on" : "off", g.cleared, g.expired,
                     g.folded_v_abs_max_m, g.bound_amb_held, g.bound_iono_held,
                     g.bound_los_epochs);
            log->info("Network (gross-LOS, RF-3b B3 emit_exclude = {}): {} satellite-epoch(s) "
                      "lost a declared line of sight from their contributor set at the "
                      "DECLARING epoch — {} withheld for want of {} contributing stations, "
                      "{} emitted with a per-satellite discontinuity mark.  ⚠ The emitted "
                      "clock and code bias are joint-solve states, so the VALUE is not "
                      "re-solved without the line of sight; what lands on the declaring "
                      "epoch is the contributor-set exclusion and its declaration.",
                      gnc3.gross_los_emit_exclude ? "on" : "off", g.emit_excluded,
                      g.emit_withheld, gnc3.gross_los_sat_min_stations,
                      g.emit_disc_marked);
            log->info("Network (gross-LOS, RF-3b B4 station_tier = {}): {} STATION "
                      "declaration(s), {} release(s), {} station-epoch(s) spent declared, "
                      "against {} whole-station guard firing(s).  With the tier off a guard "
                      "firing only defers: the next epoch declares the same lines of sight "
                      "one by one on the same evidence.",
                      gnc3.gross_los_station_tier ? "on" : "off", g.sta_declared,
                      g.sta_released, g.sta_los_epochs, g.standdowns);
            if (g.declared > 0)
                log->info("Network (gross-LOS, RF-3b B5 latency): per-episode, the "
                          "declaration landed a median-free mean of {:.2f} screened epoch(s) "
                          "after the first row over the bar (max {}).  The wire's own "
                          "latency is {} epoch(s) by construction on this arm.",
                          static_cast<double>(g.declare_latency_sum_ep)
                              / static_cast<double>(g.declared),
                          g.declare_latency_max_ep,
                          gnc3.gross_los_emit_exclude ? 0 : 1);
            if (!g.sta_declared_by_station.empty()) {
                std::string sline;
                for (const auto& [nm, n] : g.sta_declared_by_station)
                    sline += fmt::format("{}{}={}", sline.empty() ? "" : " ", nm, n);
                log->info("Network (gross-LOS, RF-3b B4): station declarations by station: {}",
                          sline);
            }
        }
        if (g.declared > 0 && g.rows_deweighted == 0)
            log->warn("Network (gross-LOS screen): {} declaration(s) but ZERO code rows were "
                      "ever folded at the deweighted σ.  The actuator lives in "
                      "networkSplitStation_ and keys on em.signal_types / em.satellite_ids; a "
                      "zero here means the declared lines of sight never returned, or the "
                      "publish half did not reach the assembly.",
                      g.declared);
        if (g.los_screened > 0) {
            const double frac = 100.0 * static_cast<double>(g.los_over)
                              / static_cast<double>(g.los_screened);
            if (frac > 5.0)
                log->warn("Network (gross-LOS screen): {:.2f} % of LOS-epochs cleared the bar "
                          "— above the 5 % line §24.18 froze as 'the threshold is wrong'.  "
                          "Report the |v|/σ distribution over all lines of sight before "
                          "touching network.gross_los_k; do NOT tune it blind.", frac);
            else
                log->info("Network (gross-LOS screen): {:.3f} % of LOS-epochs cleared the bar "
                          "(§24.18's threshold sanity line is 5 %).", frac);
        }
        // Identity tables: which stations and satellites were declared, and where the
        // screen stood down instead (invariant #7: an edge without an identity is not an
        // event).
        auto topTable = [&](const char* what, const char* axis,
                            const std::map<std::string, long long>& m) {
            if (m.empty()) return;
            std::vector<std::pair<std::string, long long>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](const auto& a2, const auto& b2) {
                return a2.second != b2.second ? a2.second > b2.second : a2.first < b2.first;
            });
            std::string line;
            std::size_t n = 0;
            for (const auto& kv : v) {
                if (n++ >= 12) { line += " …"; break; }
                line += fmt::format("{}{}={}", line.empty() ? "" : " ", kv.first, kv.second);
            }
            log->info("Network (gross-LOS screen): {} by {} ({} distinct): {}",
                      what, axis, v.size(), line);
        };
        // --- Whole-satellite stand-down (§24.19, GL-2) -------------------------
        if (ctx.knobs.gl2_on) {
            log->info("Network (gross-LOS, GL-2 whole-SATELLITE stand-down): {} satellite "
                      "stand-down edge(s) and {} release(s) over {} epoch(s) with at least "
                      "one satellite down ({} the most simultaneously); it refused {} "
                      "per-LOS declaration(s) and RELEASED {} standing one(s), and the wire "
                      "withhold pruned {} satellite-epoch(s) from an emission.",
                      g.sat_standdowns, g.sat_released, g.sat_epochs, g.sat_epoch_max,
                      g.sat_los_suppressed, g.sat_los_released, g.sat_wire_withheld);
            if (g.sat_standdowns == 0)
                log->info("Network (gross-LOS, GL-2): ZERO OPPORTUNITY — no satellite was "
                          "ever over the {:.1f}σ bar at {} or more stations in one epoch.  "
                          "A clean window, not a demonstration: the guard's power is "
                          "unproven until a window that carries a satellite-level fault "
                          "grades it (runs/_gl_on6 at TOW 271200 carries one, G17 at ten "
                          "stations).",
                          config_.getNetworkConfig().gross_los_k,
                          config_.getNetworkConfig().gross_los_sat_min_stations);
            if (g.sat_standdowns > 0 && g.sat_wire_withheld == 0
                && config_.getNetworkConfig().gross_los_sat_wire_withhold)
                log->warn("Network (gross-LOS, GL-2): {} satellite(s) stood down but NOTHING "
                          "was ever pruned from an emission, though the wire withhold is "
                          "armed.  The prune lives in networkSolPrep and keys on "
                          "ctx.carry.gl2_wire_pending; a zero here means the stood-down "
                          "satellites were never in a solution, or the pending set did not "
                          "reach the emit seam.",
                          g.sat_standdowns);
            if (g.sat_epochs > 0) {
                const double per_hour = static_cast<double>(g.sat_standdowns)
                                      / std::max(1.0, static_cast<double>(g.epochs) / 120.0);
                if (per_hour > 2.0)
                    log->warn("Network (gross-LOS, GL-2): {:.2f} satellite stand-down(s) per "
                              "hour — above the 2/hour line §24.19 froze.  Satellite-level "
                              "faults are COMMON on this bed, and the wire's per-satellite "
                              "EMISSION POLICY (not this screen) is the next card; report "
                              "the satellites and the hours.", per_hour);
                else
                    log->info("Network (gross-LOS, GL-2): {:.2f} satellite stand-down(s) per "
                              "hour (§24.19's line is 2/hour).", per_hour);
            }
            topTable("GL-2 satellite stand-downs", "satellite", g.sat_standdown_by_sat);
        }
        topTable("declarations", "station", g.declared_by_station);
        topTable("declarations", "satellite", g.declared_by_sat);
        topTable("stand-down station-epochs", "station", g.standdown_by_station);
    }
    return 0;
}

// Run-end report: the gross line-of-sight screen's cold-start bound.
int networkGrossLosColdBoundRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    networkGrossLosColdBoundRunEnd(ctx, config_, log);   // card ICF response 1
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
