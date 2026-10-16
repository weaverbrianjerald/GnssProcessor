/**
 * @file AmbStateTrace.cpp
 * @brief Row formatting and the serial drain for @c network_amb_state.csv
 *        (RF-1 §7.4).  @see include/network/diag/AmbStateTrace.hpp
 */
#include "network/diag/AmbStateTrace.hpp"

#include "network/state/RunContext.hpp"
#include "network/diag/RunTraces.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

// The run-end reports at the end of this file:
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include <memory>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/carry/LocalCarryHarvest.hpp"   // HarvestBorder, StationHarvestBlock
#include "common/GnssIdentity.hpp"         // systemOfSat, traitsOf, SignalId, nominalCarrierFrequencyHz
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "network/ar/AmbFloat.hpp"       // ambFloatBirthFloor (RF-1 step 4)
#include "network/carry/CarryVce.hpp"
#include "network/assembly/ParamInventory.hpp" // rxIfbFamilyReseeded
#include "network/qc/PriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "network/solve/NormalEquations.hpp"
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan
#include "filter/StateKeySchema.hpp"         // StateKeys: ambiguity and phase-bias keys, GLONASS IFB keys
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {

const char* ambStateRouteName(AmbFoldRoute r) {
    switch (r) {
        case AmbFoldRoute::Diag:           return "diag";
        case AmbFoldRoute::Block:          return "block";
        case AmbFoldRoute::CouplingScalar: return "coupling-scalar";
        case AmbFoldRoute::None:
        default:                           return "";
    }
}

std::string ambStateFormatRow(const AmbStateRow& r) {
    // {:.17g} on the graded quantities (the value and the four variances): D1 fits a
    // slope through fold_var over 30 folds at the 1e-4 m² scale and D2 compares
    // value_abs against a 0.3 m bar, so a 6-significant-digit default would quantize
    // the signal the reads are built on.  TOW keeps 3 decimals: it is the join key of
    // every other trace CSV in the run and must match them cell for cell.
    return fmt::format("{},{:.3f},{},{},{},"
                       "{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},"
                       "{},{},{},"
                       "{},{},"
                       "{}\n",
                       r.week, r.tow, r.station, r.sat, r.signal,
                       r.value_abs_m, r.var_m2, r.var_first_m2, r.var_birth_m2,
                       r.fold_var_m2,
                       r.n_folds, r.age_ep, r.age_since_relevel_ep,
                       r.folded ? 1 : 0, ambStateRouteName(r.route),
                       r.event);
}

void ambStateNoteErase(NetworkRunContext& ctx, long long week, double tow,
                       const std::string& station, const std::string& sat,
                       const std::string& signal, const LocalCarry& lc,
                       const char* cause) {
    if (!ctx.knobs.trace_amb_state) return;
    AmbStateRow row;
    row.week = week;
    row.tow  = tow;
    row.station = station;
    row.sat     = sat;
    row.signal  = signal;
    // The entry as it stands at deletion: its last harvested value and variance, its
    // birth prior, and its fold count.  fold_var / folded / route keep their defaults
    // because an erase row is not a fold; D3 reads the "variance before" from var_m2,
    // which is the variance the next arc starts over from.
    row.value_abs_m  = lc.value_abs;
    row.var_m2       = lc.var;
    row.var_first_m2 = lc.var_first;
    row.var_birth_m2 = lc.var_birth;
    row.n_folds      = lc.n_folds;
    row.age_ep = (lc.birth_ep > 0)
        ? (static_cast<long long>(ctx.counters.n_loop_epochs) - lc.birth_ep)
        : lc.birth_ep;
    row.age_since_relevel_ep = -1;   // no epoch clock at this site; the harvest owns it
    row.event = std::string("erase:") + cause;
    ctx.carry.amb_state_rows.push_back(std::move(row));
}

void ambStateNoteDrop(NetworkRunContext& ctx, long long week, double tow,
                      const std::string& station, const std::string& sat,
                      const std::string& signal, const LocalCarry& lc,
                      const char* cause) {
    if (!ctx.knobs.trace_amb_state) return;
    AmbStateRow row;
    row.week = week;
    row.tow  = tow;
    row.station = station;
    row.sat     = sat;
    row.signal  = signal;
    // Post-drop state: D3 reads the restored variance as "after".  Design §3.4 treats
    // it as a stated, bounded level, unlike the missing information a free epoch or an
    // erase leaves behind.
    row.value_abs_m  = lc.value_abs;     // kept: a drop changes confidence, not identity
    row.var_m2       = lc.var;
    row.var_first_m2 = lc.var_first;
    row.var_birth_m2 = lc.var_birth;
    row.n_folds      = lc.n_folds;
    row.age_ep = (lc.birth_ep > 0)
        ? (static_cast<long long>(ctx.counters.n_loop_epochs) - lc.birth_ep)
        : lc.birth_ep;
    row.age_since_relevel_ep = 0;        // the drop is the re-level event
    row.event = std::string("drop:") + cause;
    ctx.carry.amb_state_rows.push_back(std::move(row));
}

void networkAmbStateTrace(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    // ---- End of the bindings ----
    // --- alias preamble (the seam contract): reconstruct the driver's names -------
    auto& rows     = ctx.carry.amb_state_rows;
    auto& counters = ctx.counters;

    if (rows.empty()) return;
    if (!traces.amb_state_trace.is_open()) {
        // openAll() opens the stream once under the same knob, so a closed stream here
        // means the open failed (openAll() already warned).  Clear the buffer anyway,
        // so it does not grow for the whole run.
        if (log) log->warn("Network (RF-1 §7.4): {} buffered carry-state row(s) discarded — "
                           "network_amb_state.csv is not open.", rows.size());
        rows.clear();
        return;
    }
    long long events = 0, erases = 0;
    for (const AmbStateRow& r : rows) {
        traces.amb_state_trace << ambStateFormatRow(r);
        if (!r.event.empty()) {
            ++events;
            if (r.event.rfind("erase:", 0) == 0) ++erases;
        }
    }
    counters.n_amb_state_rows       += static_cast<long long>(rows.size());
    counters.n_amb_state_events     += events;
    counters.n_amb_state_erase_rows += erases;
    ++counters.n_amb_state_epochs;
    rows.clear();
}

// ---- The buffered-row drains of the phase-colour and IF-ambiguity traces (frozen text of
//      Application::networkProcessEpoch_, moved by scripts/nom_ep2_cut.py) ----
void networkBufferedTraceDrains(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep2_cut.py,
    //      each line checked by scripts/nom_ep2_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunTraces& traces = run.traces;
    const EpochEvents::Harvest& harvest_ev = ep.events.harvest;
    // ---- End of the bindings ----
    // Card LGM: write the phase-colour trace rows buffered at the harvest.
    if (traces.phase_colour_trace.is_open())
        for (const std::string& row : harvest_ev.pc_trace_rows)
            traces.phase_colour_trace << row;
    // Card AIL: the same for the ambiguity information trace.
    if (traces.amb_if_trace.is_open())
        for (const std::string& row : harvest_ev.amb_if_rows)
            traces.amb_if_trace << row;
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the ambiguity-state trace.
int networkAmbStateTraceRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (config_.getNetworkConfig().trace_amb_state)
        // "0 rows under an armed trace" logs at warn level, not as a clean line
        // (invariant #4).  On this vehicle the trace rides the leveling harvest, so 0 rows
        // means the carry never ran (carry_warm never reached, or carry_invariant) rather
        // than a quiet network.  The erase split is reported beside the total because it
        // is the whole of D3's inventory, and because a run with births and trajectory
        // rows but no erase rows over a 60-station field hour would itself be a finding
        // (the four erase paths are the population §3.4 replaces).
        log->log(ctx.counters.n_amb_state_rows > 0 ? spdlog::level::info : spdlog::level::warn,
                 "Network (RF-1 §7.4 carry-state trace): {} row(s) written to "
                 "network_amb_state.csv over {} traced epoch(s) — {} carrying an event, "
                 "of which {} are erase: rows (t4_attrib / t3_consensus / gap_prune / "
                 "anchor_reref, each captured before the entry was deleted).  0 rows "
                 "under an ARMED trace means the leveling harvest never ran — read the "
                 "carry warm-up and the vehicle, not this line.  Reads: "
                 "scripts/amb_state_read.py (D1 the variance trajectory, D2 the level "
                 "trajectory, D3 this inventory, D4 σ̂₀(pl) by arc age).",
                 ctx.counters.n_amb_state_rows, ctx.counters.n_amb_state_epochs,
                 ctx.counters.n_amb_state_events, ctx.counters.n_amb_state_erase_rows);
    return 0;
}

// ---- End of the run-end reports ----

// ---- Harvest half (generated by scripts/nom_hv1_cut.py) ----

void networkAmbIfTraceStation(NetworkRun& run, const NetworkEpoch& ep,
                              const StationHarvestBlock& sb, std::size_t i, EpochEvents& events) {
    // ---- From the run, the epoch and the harvest (generated by
    //      scripts/nom_hv1_cut.py, each line checked by scripts/nom_hv1_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    double pl_now_s = ep.clock.pl_now_s;
    const StationAssembly& R = asm_res[i];
    const std::shared_ptr<PlHarvestBlock>& hblk = sb.blk;
    EpochEvents::Harvest& harvest_ev = events.harvest;   // written
    // ---- End of the bindings ----
    // Card AIL (network.trace_amb_if, trace class): the ionosphere-free ambiguity's
    // variance before and after this epoch, per line of sight whose two bands are
    // both carried in the block.  Prior: the joint covariance the inject folded
    // (R.pl_blk.Sigma, rows keyed by local column); posterior: the harvest block
    // just filled (rows a of R.pl_amb).  Both are conditional on the border.  The
    // demeaned versions project out the station's common mode over the traced lines
    // of sight of the same constellation (the direction the free receiver clock and
    // that constellation's receiver biases leave to the code).  Reads only; nothing
    // the solve uses is written.
    if (!(ctx.knobs.amb_if_trace && hblk && !R.pl_amb.empty())) return;
    // The ionosphere-free pair of a line of sight: the first two bands of its
    // constellation in RINEX digit order (the traits table: GPS L1/L2, Galileo
    // E1/E5a, QZSS L1/L2, SBAS L1/L5), with the IF coefficients from their
    // carriers.  BeiDou is not traced: its first two bands, B1C and B1I, are
    // 14 MHz apart and form no usable pair (the coefficient is about 55); its
    // pair is a band-role decision.  GLONASS is never
    // carried in the block; its FDMA bands would be read at slot 0.
    struct IfPair { char d1{'\0'}, d2{'\0'}; double ca{0.0}, cb{0.0}; };
    const auto ifPairOf = [](System sys) {
        IfPair pr;
        const ConstellationTraits& t = traitsOf(sys);
        if (sys == System::BeiDou || t.n_bands < 2) return pr;
        const double f1 = nominalCarrierFrequencyHz(t.bands[0].band);
        const double f2 = nominalCarrierFrequencyHz(t.bands[1].band);
        pr.d1 = t.bands[0].digit;
        pr.d2 = t.bands[1].digit;
        pr.ca = f1 * f1 / (f1 * f1 - f2 * f2);
        pr.cb = -f2 * f2 / (f1 * f1 - f2 * f2);
        return pr;
    };
    std::map<std::string, std::pair<int, int>> los;   // sat -> (a band 1, a band 2)
    std::map<std::string, IfPair> if_pair;            // sat -> its pair
    for (std::size_t a = 0; a < R.pl_amb.size(); ++a) {
        const System sys = systemOfSat(R.pl_amb[a].sat);
        const auto sig = SignalId::fromObsCode(sys, R.pl_amb[a].signal);
        if (!sig) continue;
        const IfPair& pr =
            if_pair.try_emplace(R.pl_amb[a].sat, ifPairOf(sys)).first->second;
        if (pr.d1 == '\0') continue;
        auto& lp = los.try_emplace(R.pl_amb[a].sat, -1, -1).first->second;
        if (sig->digit == pr.d1 && lp.first < 0) lp.first = static_cast<int>(a);
        else if (sig->digit == pr.d2 && lp.second < 0) lp.second = static_cast<int>(a);
    }
    std::map<Eigen::Index, Eigen::Index> prow;   // local column -> prior block row
    for (std::size_t k = 0; k < R.pl_blk.rows.size(); ++k)
        prow[R.pl_blk.rows[k].local_col] = static_cast<Eigen::Index>(k);
    // The traced lines of sight, one group per constellation: each constellation
    // has its own receiver code and phase biases, so its common mode is its own.
    // `los` is in satellite-string order, so a constellation is a contiguous run.
    struct TracedLos { std::string sat; std::array<int, 2> idx; IfPair coef; };
    std::vector<std::vector<TracedLos>> groups;
    System group_sys = System::Unknown;
    for (const auto& [sat, lp] : los) {
        if (lp.first < 0 || lp.second < 0) continue;
        const System sys = systemOfSat(sat);
        if (groups.empty() || sys != group_sys) {
            groups.emplace_back();
            group_sys = sys;
        }
        groups.back().push_back({sat, {lp.first, lp.second}, if_pair.at(sat)});
    }
    const auto post = [&](int a, int b) {
        const double u = hblk->C(a, b), v = hblk->C(b, a);
        return std::isfinite(u) && std::isfinite(v) ? 0.5 * (u + v)
             : std::numeric_limits<double>::quiet_NaN();
    };
    const auto prior = [&](int a, int b) {
        const auto ia = prow.find(R.pl_amb[static_cast<std::size_t>(a)].col);
        const auto ib = prow.find(R.pl_amb[static_cast<std::size_t>(b)].col);
        if (ia == prow.end() || ib == prow.end())
            return std::numeric_limits<double>::quiet_NaN();
        return R.pl_blk.Sigma(ia->second, ib->second);
    };
    const double t_week = std::floor(pl_now_s / 604800.0);
    const double t_tow  = pl_now_s - 604800.0 * t_week;
    for (const std::vector<TracedLos>& g : groups) {
        const Eigen::Index n = static_cast<Eigen::Index>(g.size());
        const auto ifcov = [&](const auto& c, Eigen::Index r, Eigen::Index s) {
            const auto& p = g[static_cast<std::size_t>(r)].idx;
            const auto& q = g[static_cast<std::size_t>(s)].idx;
            const IfPair& u = g[static_cast<std::size_t>(r)].coef;
            const IfPair& v = g[static_cast<std::size_t>(s)].coef;
            return u.ca * v.ca * c(p[0], q[0]) + u.ca * v.cb * c(p[0], q[1])
                 + u.cb * v.ca * c(p[1], q[0]) + u.cb * v.cb * c(p[1], q[1]);
        };
        Eigen::MatrixXd Vpo(n, n), Vpr(n, n);
        for (Eigen::Index r2 = 0; r2 < n; ++r2)
            for (Eigen::Index c2 = 0; c2 < n; ++c2) {
                Vpo(r2, c2) = ifcov(post, r2, c2);
                Vpr(r2, c2) = ifcov(prior, r2, c2);
            }
        const Eigen::MatrixXd Pm = Eigen::MatrixXd::Identity(n, n)
            - Eigen::MatrixXd::Constant(n, n, 1.0 / static_cast<double>(n));
        const bool po_ok = Vpo.allFinite(), pr_ok = Vpr.allFinite();
        const Eigen::MatrixXd Dpo = po_ok ? Eigen::MatrixXd(Pm * Vpo * Pm) : Vpo;
        const Eigen::MatrixXd Dpr = pr_ok ? Eigen::MatrixXd(Pm * Vpr * Pm) : Vpr;
        for (Eigen::Index r2 = 0; r2 < n; ++r2) {
            const TracedLos& t = g[static_cast<std::size_t>(r2)];
            const auto lit = net::getArc(ctx.entities.arcs, std::make_tuple(
                static_cast<int>(i), t.sat,
                R.pl_amb[static_cast<std::size_t>(t.idx[0])].signal), &net::ArcState::amb);
            const long long age = (lit != nullptr && lit->birth_ep > 0)
                ? static_cast<long long>(ctx.counters.n_loop_epochs) - lit->birth_ep
                : -1;
            harvest_ev.amb_if_rows.push_back(fmt::format(
                "{:.0f},{:.1f},{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e}\n",
                t_week, t_tow, stations[i].name, t.sat,
                age, n, Vpr(r2, r2), Vpo(r2, r2),
                pr_ok ? Dpr(r2, r2) : std::numeric_limits<double>::quiet_NaN(),
                po_ok ? Dpo(r2, r2) : std::numeric_limits<double>::quiet_NaN()));
        }
    }
}

// ---- End of the harvest half ----

}  // namespace GnssProcessor
