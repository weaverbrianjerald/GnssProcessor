/**
 * @file NetworkFixedCycles.cpp
 * @brief The fixed-cycle fold (card FCF): the units, the per-unit test, the ledger's units and
 *        their re-validation, the placement, the fold with its acceptance passes, the fold's
 *        releases and edges, the seam that steps the ledger and the fold and writes their
 *        traces, and its run-end report.  What it does and why the estimator stays float are in
 *        the header's file banner, include/filter/NetworkFixedCycles.hpp.
 */

#include "filter/NetworkFixedCycles.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.carry.ar_lattice / fixed_cycles, ctx.counters, ctx.inject
#include "config/GnssConfig.hpp"         // network.ambiguity_resolution.fixed_cycles.*
#include "filter/NetworkArcGraph.hpp"    // arcgraph::Dsu (the blocks are station components), the lattice
#include "filter/NetworkArcLife.hpp"     // kArcLifeFlagEp: a post-fit flag read through the stamp
#include "filter/NetworkRunEnd.hpp"      // NetworkRunEndView
#include "filter/NetworkRunTypes.hpp"    // NetStation, WithinEpochCulprits
#include "logging/NetworkRunTraces.hpp"  // fixed_cycles_trace, fixed_cycle_shift_trace, fixed_cycle_events_trace
#include "math/RobustStats.hpp"          // RobustStats::median
#include "products/NetworkProductEmitter.hpp"  // NetworkSolutionEpoch (the shift trace)
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "app/NetworkSetupResult.hpp"

namespace GnssProcessor {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// The regularized lower incomplete gamma function P(a, x): the series below a + 1, the
/// continued fraction (modified Lentz) above it.
double gammaP(double a, double x) {
    if (x <= 0.0) return 0.0;
    const double lpre = a * std::log(x) - x - std::lgamma(a);
    if (x < a + 1.0) {
        double ap = a, del = 1.0 / a, sum = del;
        for (int n = 0; n < 100000; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::abs(del) < std::abs(sum) * 1e-16) break;
        }
        return std::min(1.0, sum * std::exp(lpre));
    }
    const double tiny = 1e-300;
    double b = x + 1.0 - a, c = 1.0 / tiny, d = 1.0 / b, h = d;
    for (int i = 1; i < 100000; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::abs(d) < tiny) d = tiny;
        c = b + an / c;
        if (std::abs(c) < tiny) c = tiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::abs(del - 1.0) < 1e-16) break;
    }
    return std::max(0.0, 1.0 - std::exp(lpre) * h);
}

/// A sample's quantile @p q (nearest rank on the sorted copy); NaN when empty.
double quantileOf(std::vector<double> v, double q) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const std::size_t k = std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())));
    return v[k];
}

/// The largest element of a sample; NaN when empty.
double maxOf(const std::vector<double>& v) {
    return v.empty() ? kNaN : *std::max_element(v.begin(), v.end());
}

/// The wavelength of a DD's band (m).
double lambdaOf(const SearchedCycle& s) { return std::get<0>(s.id).wavelengthM(0); }

/// A station's name by roster index ("?" outside the roster).
std::string stationName(const std::vector<NetStation>& stations, const StationId& s) {
    return (s.index >= 0 && static_cast<std::size_t>(s.index) < stations.size())
               ? stations[static_cast<std::size_t>(s.index)].name : std::string("?");
}

/// The epoch's rows of network_fixed_cycles.csv (kFixedCyclesHeader), one per unit.
void writeUnitRows(std::ofstream& os, const SyncedEpoch& se, const std::vector<NetStation>& stations,
                   const ArEpochFixes& fixes, const std::vector<fcyc::Unit>& units,
                   const fcyc::EpochFold& f) {
    for (std::size_t u = 0; u < units.size(); ++u) {
        const fcyc::Unit& un = units[u];
        const SearchedCycle& a = fixes.floats[static_cast<std::size_t>(un.l1)];
        const SearchedCycle& b = fixes.floats[static_cast<std::size_t>(un.l2)];
        const auto& [sig1, sta_r, ref_sta, sat_j, ref_sat] = a.id;
        const SignalId& sig2 = std::get<0>(b.id);
        const double sd1 = std::sqrt(std::max(0.0, a.var_cyc2));
        const double sd2 = std::sqrt(std::max(0.0, b.var_cyc2));
        const double rho = (sd1 > 0.0 && sd2 > 0.0) ? a.cov_twin_cyc2 / (sd1 * sd2) : kNaN;
        const fcyc::UnitFate& fa = f.fate[u];
        os << fmt::format("{},{:.1f},{},{},{},{},{},{},{},{},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},"
                          "{:.4f},{},{},{:.4f},{:.4f},{:.4f},{},{}\n",
                          se.time.week, se.time.tow, constellationKey(sig1.sys),
                          sig1.obsCode(ObsType::Phase), sig2.obsCode(ObsType::Phase), sat_j.str(),
                          ref_sat.str(), stationName(stations, sta_r), stationName(stations, ref_sta),
                          un.n1, un.n2, a.y_cyc, b.y_cyc, sd1, sd2, rho, un.t, fa.block, fa.fig.k,
                          fa.fig.chi2, fa.fig.chi2_expect, fa.fig.bar, fa.pass,
                          fcyc::unitActionName(fa.action));
    }
}

/// The epoch's rows of network_fixed_cycle_shift.csv (kFixedCycleShiftHeader): held minus
/// float on every emitted clock, code bias, phase bias and phase-visible correction (phase bias
/// minus clock, the phase side a consumer applies).
void writeShiftRows(std::ofstream& os, const SyncedEpoch& se, const NetworkEstimator& est,
                    const Eigen::VectorXd& x_float, const Eigen::VectorXd& x_held) {
    const NetworkSolutionEpoch fl = est.toSolutionEpoch(x_float, se.time);
    const NetworkSolutionEpoch he = est.toSolutionEpoch(x_held, se.time);
    std::map<std::string, const NetworkSatSolution*> held_of;
    for (const NetworkSatSolution& s : he.sats) held_of[s.sat] = &s;
    auto row = [&](const std::string& sat, const char* kind, const std::string& sig, double f, double h) {
        os << fmt::format("{},{:.1f},{},{},{},{:.6f},{:.6f},{:.6f}\n", se.time.week, se.time.tow, sat,
                          kind, sig, f, h, h - f);
    };
    for (const NetworkSatSolution& s : fl.sats) {
        const auto it = held_of.find(s.sat);
        if (it == held_of.end()) continue;
        const NetworkSatSolution& h = *it->second;
        if (s.has_clock && h.has_clock) row(s.sat, "clock", "", s.clock_c0_m, h.clock_c0_m);
        for (const auto& [sig, v] : s.code_bias_m) {
            const auto hc = h.code_bias_m.find(sig);
            if (hc != h.code_bias_m.end()) row(s.sat, "code_bias", sig, v, hc->second);
        }
        for (const auto& [sig, pb] : s.phase_bias) {
            const auto hp = h.phase_bias.find(sig);
            if (hp == h.phase_bias.end()) continue;
            row(s.sat, "phase_bias", sig, pb.bias_m, hp->second.bias_m);
            if (s.has_clock && h.has_clock)
                row(s.sat, "phase_visible", sig, pb.bias_m - s.clock_c0_m, hp->second.bias_m - h.clock_c0_m);
        }
    }
}

/// "name n, ..." over the non-zero entries of the action tally, or "none".
std::string actionText(const std::array<long long, fcyc::kUnitActionCount>& counts) {
    std::string out;
    for (std::size_t k = 0; k < counts.size(); ++k) {
        if (counts[k] == 0) continue;
        out += fmt::format("{}{} {}", out.empty() ? "" : ", ",
                           fcyc::unitActionName(static_cast<fcyc::UnitAction>(k)), counts[k]);
    }
    return out.empty() ? std::string("none") : out;
}

/// The eight band-arcs of a unit (its two cycles' terms), in cycle order.
std::vector<ArcId> unitArcs(const fcyc::Unit& u, const ArEpochFixes& fixes) {
    std::vector<ArcId> arcs;
    for (int k : {u.l1, u.l2})
        for (const arcgraph::CycleTerm& t : fixes.floats[static_cast<std::size_t>(k)].cycle.terms)
            arcs.push_back(t.arc);
    return arcs;
}

/// The knobs of one fold, from the configuration.
fcyc::FoldKnobs foldKnobsOf(const NetworkArConfig& ar) {
    fcyc::FoldKnobs k;
    k.sigma_cyc   = ar.fixed_cycle_sigma_cyc;
    k.gate_alpha  = ar.fixed_cycle_gate_alpha;
    k.max_iter    = ar.fixed_cycle_max_iter;
    k.max_shift_m = ar.fixed_cycle_max_shift_m;
    return k;
}

/// The arc a planted integer names, or none when its station, satellite or signal does not
/// resolve.
std::optional<ArcId> plantArc(const InjectFixedCyclePlant& p, const std::vector<NetStation>& stations) {
    int station = -1;
    for (std::size_t i = 0; i < stations.size(); ++i)
        if (stations[i].name == p.station) station = static_cast<int>(i);
    const std::optional<SatId> sat = SatId::parse(p.sat);
    if (station < 0 || !sat) return std::nullopt;
    const std::optional<SignalId> sig = SignalId::fromObsCode(sat->sys, p.signal);
    if (!sig) return std::nullopt;
    return ArcId{StationId{station}, *sat, *sig};
}

/// The event harness's planted integers due at this epoch (file banner, source lattice step 2):
/// each fires at the first epoch the ledger holds its arc, and closes its injector entry.
void plantFixedCycles(NetworkRunContext& ctx, const std::shared_ptr<spdlog::logger>& log,
                      NetworkRunTraces& traces, const SyncedEpoch& se,
                      const std::vector<NetStation>& stations, arcgraph::CycleLattice& L) {
    NetworkInjectState& inj = ctx.inject;
    if (!inj.armed) return;
    for (const InjectFixedCyclePlant& p : inj.fixed_cycle_plant_now) {
        const std::size_t k = p.ev_index;
        if (k >= inj.events.size() || inj.onset_logged[k]) continue;
        const std::optional<ArcId> arc = plantArc(p, stations);
        if (!arc) continue;
        const int rows = arcgraph::latticeShiftArc(L, *arc, p.delta);
        if (rows == 0) continue;   // not held yet: re-staged at the next epoch
        ++inj.n_applied[k];
        inj.onset_logged[k] = true;
        ++inj.fired_by_kind["fixed_cycle_plant"];
        ++ctx.counters.fixed_cycles.plants;
        traces.emitEvent(se.time, "INJECT", "fixed_cycle_plant", p.sat, p.station,
                         static_cast<double>(p.delta), std::nullopt, "injected",
                         fmt::format("signal={};rows={}", p.signal, rows));
        log->warn("Network (fault inject): TOW {:.1f} fixed_cycle_plant FIRED: {} cycle(s) planted "
                  "on {} {} {} in {} row(s) of the ledger of fixed cycles.",
                  se.time.tow, p.delta, p.station, p.sat, p.signal, rows);
    }
}

/// True when the search determined the unit's pair at this epoch (both bands).
bool searchKept(const fcyc::Unit& u, const ArEpochFixes& fixes) {
    return fixes.floats[static_cast<std::size_t>(u.l1)].fixed && fixes.floats[static_cast<std::size_t>(u.l2)].fixed;
}

/// Add one fold to the run tallies.
void tallyFold(FixedCycleCounters& C, const std::vector<fcyc::Unit>& units, const fcyc::EpochFold& f,
               const ArEpochFixes& fixes) {
    C.units  += static_cast<long long>(units.size());
    C.passes += f.passes;
    for (const fcyc::Unit& u : units) C.unit_t.push_back(u.t);
    for (const fcyc::UnitFate& fa : f.fate) ++C.by_action[static_cast<std::size_t>(fa.action)];
    for (std::size_t u = 0; u < units.size() && u < f.fate.size(); ++u)
        if (f.fate[u].action == fcyc::UnitAction::Folded && !searchKept(units[u], fixes)) ++C.folded_unkept;
    if (f.passes > 0 && f.error.empty()) {
        C.repro.push_back(f.held.float_repro_rel);
        C.factor_nnz_max = std::max(C.factor_nnz_max, f.held.factor_nnz);
        for (std::size_t b = 0; b < f.held.degenerate.size(); ++b) {
            if (!f.held.degenerate[b]) continue;
            ++C.blocks_degenerate;
            if (f.held.dependent[b]) ++C.blocks_dependent;
        }
    }
    C.blocks_failed += f.failures;
    if (f.accepted) {
        ++C.held_solves;
        C.shift_m.push_back(f.held.max_shift_m);
        long long folded = 0;
        for (std::size_t b = 0; b < f.blocks.size(); ++b) {
            if (f.held.degenerate[b]) continue;
            const fcyc::BlockFigures& fig = f.blocks[b];
            ++C.blocks_folded;
            folded += static_cast<long long>(f.plan.block_units[b].size());
            C.block_k.push_back(static_cast<double>(fig.k));
            C.chi2_ratio.push_back(fig.chi2_expect > 0.0 ? fig.chi2 / fig.chi2_expect : kNaN);
            C.expect_per_dof.push_back(fig.k > 0 ? fig.chi2_expect / static_cast<double>(fig.k) : kNaN);
        }
        C.units_per_epoch.push_back(static_cast<double>(folded));
    } else {
        ++C.epochs_refused;
    }
}

/// One row of network_fixed_cycle_events.csv (kFixedCycleEventsHeader).
void writeEdgeRow(std::ofstream& os, const SyncedEpoch& se, const std::vector<NetStation>& stations,
                  const char* event, const ArDdIdentity& id, const FixedCycleRun& run, long long run_ep,
                  const std::string& cause, const std::string& detail) {
    const auto& [sig1, sta_r, ref_sta, sat_j, ref_sat] = id;
    os << fmt::format("{},{:.1f},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", se.time.week, se.time.tow, event,
                      constellationKey(sig1.sys), sig1.obsCode(ObsType::Phase),
                      run.sig2.obsCode(ObsType::Phase), sat_j.str(), ref_sat.str(),
                      stationName(stations, sta_r), stationName(stations, ref_sta), run.n1, run.n2, run_ep,
                      cause, detail);
}

/// Why a folded run ended at this epoch (the offset row's cause).
std::string offsetCause(const ArDdIdentity& id, const FixedCycleRun& run, bool contiguous,
                        const std::map<ArDdIdentity, int>& unit_of, const fcyc::EpochFold& f,
                        const ArEpochFixes& fixes, const ArLatticeEpoch& le) {
    if (!contiguous) return "seam_gap";
    const auto u = unit_of.find(id);
    if (u != unit_of.end() && static_cast<std::size_t>(u->second) < f.fate.size()) {
        const fcyc::UnitAction a = f.fate[static_cast<std::size_t>(u->second)].action;
        return a == fcyc::UnitAction::Folded ? std::string("integers_changed")
                                             : std::string(fcyc::unitActionName(a));
    }
    auto onRun = [&run](const ArcId& a) { return std::find(run.arcs.begin(), run.arcs.end(), a) != run.arcs.end(); };
    for (const ArLatticeReleaseEvent& r : le.releases) {
        const bool hit = (r.scope == ArLatticeScope::Arc && onRun(r.arc))
                      || (r.scope == ArLatticeScope::Cycle
                          && std::any_of(r.cycle.terms.begin(), r.cycle.terms.end(),
                                         [&](const arcgraph::CycleTerm& t) { return onRun(t.arc); }))
                      || r.scope == ArLatticeScope::All;
        if (hit) return arLatticeReleaseName(r.cause);
    }
    for (const SearchedCycle& s : fixes.floats)
        if (s.id == id) return "not_implied";
    return "out_of_basis";
}

/// The fold's edges at this epoch (file banner, source lattice step 6): the late post-fit flags
/// on the units folded at the step before, then each run that ended and each that began.
void foldEdges(FixedCycleState& st, FixedCycleCounters& C, NetworkRunTraces& traces, const SyncedEpoch& se,
               const std::vector<NetStation>& stations, const ArEpochFixes& fixes,
               const std::vector<fcyc::Unit>& units, const fcyc::EpochFold& f, const ArLatticeEpoch& le,
               long long ep) {
    std::ofstream* const os = traces.fixed_cycle_events_trace.is_open() ? &traces.fixed_cycle_events_trace : nullptr;
    const bool contiguous = st.last_ep == ep - 1;
    // RAIM T3 / T4 and the gross-LOS Act act after the emission; their flag is read at this step.
    if (contiguous)
        for (const ArLatticeReleaseEvent& r : le.releases) {
            if (r.scope != ArLatticeScope::Arc || r.cause != ArLatticeRelease::StampChange
                || (r.change & kArcLifeFlagEp) == 0)
                continue;
            for (const auto& [id, run] : st.folded) {
                if (std::find(run.arcs.begin(), run.arcs.end(), r.arc) == run.arcs.end()) continue;
                ++C.late_t3t4;
                if (os) writeEdgeRow(*os, se, stations, "late_t3t4", id, run, ep - run.since, "stamp",
                                     fmt::format("{}/{}/{}", stationName(stations, r.arc.station),
                                                 r.arc.sat.str(), r.arc.signal.obsCode(ObsType::Phase)));
            }
        }
    // The units folded now; a run goes on when the step before folded it at the same integers.
    std::map<ArDdIdentity, FixedCycleRun> now;
    std::map<ArDdIdentity, int> unit_of;
    for (std::size_t u = 0; u < units.size(); ++u) {
        const fcyc::Unit& un = units[u];
        const ArDdIdentity& id = fixes.floats[static_cast<std::size_t>(un.l1)].id;
        unit_of[id] = static_cast<int>(u);
        if (u >= f.fate.size() || f.fate[u].action != fcyc::UnitAction::Folded) continue;
        FixedCycleRun run{ep, un.n1, un.n2, std::get<0>(fixes.floats[static_cast<std::size_t>(un.l2)].id),
                          unitArcs(un, fixes)};
        const auto prev = st.folded.find(id);
        if (contiguous && prev != st.folded.end() && prev->second.n1 == un.n1 && prev->second.n2 == un.n2)
            run.since = prev->second.since;
        now.emplace(id, std::move(run));
    }
    for (const auto& [id, run] : st.folded) {
        const auto it = now.find(id);
        if (it != now.end() && it->second.since == run.since) continue;   // the run goes on
        ++C.offsets;
        if (os) writeEdgeRow(*os, se, stations, "offset", id, run, ep - run.since,
                             offsetCause(id, run, contiguous, unit_of, f, fixes, le), "");
    }
    for (const auto& [id, run] : now) {
        if (run.since != ep) continue;
        ++C.onsets;
        if (os) writeEdgeRow(*os, se, stations, "onset", id, run, 0, "", "");
    }
    st.folded  = std::move(now);
    st.last_ep = ep;
}

/// Milliseconds since @p t0.
double msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

namespace fcyc {

const char* unitActionName(UnitAction a) {
    switch (a) {
    case UnitAction::Folded:        return "folded";
    case UnitAction::UnitRefused:   return "unit_refused";
    case UnitAction::Unplaced:      return "unplaced";
    case UnitAction::Dropped:       return "dropped";
    case UnitAction::LeftFloat:     return "left_float";
    case UnitAction::Degenerate:    return "degenerate";
    case UnitAction::FloatMismatch: return "float_mismatch";
    case UnitAction::NonFinite:     return "non_finite";
    case UnitAction::KernelError:   return "kernel_error";
    case UnitAction::ShiftRefused:  return "shift_refused";
    case UnitAction::Released:      return "released";
    case UnitAction::Lapsed:        return "lapsed";
    case UnitAction::Count:         break;
    }
    return "?";
}

std::vector<Unit> epochUnits(const ArEpochFixes& fixes) {
    std::vector<Unit> out;
    const int n = static_cast<int>(fixes.floats.size());
    for (int a = 0; a < n; ++a) {
        const SearchedCycle& s = fixes.floats[static_cast<std::size_t>(a)];
        const int b = s.twin;
        if (b < 0 || b >= n || !s.fixed) continue;
        const SearchedCycle& t = fixes.floats[static_cast<std::size_t>(b)];
        if (!t.fixed || t.twin != a) continue;
        // Each pair once, from its shorter-wavelength band (band 1).
        if (!(lambdaOf(s) < lambdaOf(t))) continue;
        Unit u;
        u.l1 = a;
        u.l2 = b;
        u.n1 = s.integer;
        u.n2 = t.integer;
        out.push_back(u);
    }
    return out;
}

double unitStatistic(double y1, double y2, long long n1, long long n2, double var1, double var2,
                     double cov12) {
    const double det = var1 * var2 - cov12 * cov12;
    if (!(std::isfinite(det) && var1 > 0.0 && var2 > 0.0 && det > 0.0)) return kNaN;
    const double v1 = y1 - static_cast<double>(n1);
    const double v2 = y2 - static_cast<double>(n2);
    const double t = (v1 * v1 * var2 - 2.0 * v1 * v2 * cov12 + v2 * v2 * var1) / det;
    return std::isfinite(t) ? t : kNaN;
}

void scoreUnits(std::vector<Unit>& units, const ArEpochFixes& fixes, double alpha) {
    const double bar = -2.0 * std::log(alpha);   // chi2_2(1 - alpha), exactly
    for (Unit& u : units) {
        const SearchedCycle& a = fixes.floats[static_cast<std::size_t>(u.l1)];
        const SearchedCycle& b = fixes.floats[static_cast<std::size_t>(u.l2)];
        u.t    = unitStatistic(a.y_cyc, b.y_cyc, u.n1, u.n2, a.var_cyc2, b.var_cyc2, a.cov_twin_cyc2);
        u.fail = !(u.t <= bar);   // NaN fails
    }
}

std::vector<Unit> impliedUnits(const arcgraph::CycleLattice& L, const ArEpochFixes& fixes, long long ep) {
    std::vector<Unit> out;
    if (L.rows.empty()) return out;
    const int n = static_cast<int>(fixes.floats.size());
    for (int a = 0; a < n; ++a) {
        const SearchedCycle& s = fixes.floats[static_cast<std::size_t>(a)];
        const int b = s.twin;
        if (b < 0 || b >= n) continue;
        const SearchedCycle& t = fixes.floats[static_cast<std::size_t>(b)];
        if (t.twin != a || !(lambdaOf(s) < lambdaOf(t))) continue;   // each pair once, from band 1
        long long new1 = -1, new2 = -1;
        const std::optional<long long> n1 = arcgraph::latticeImplied(L, s.cycle, &new1);
        if (!n1 || new1 >= ep) continue;
        const std::optional<long long> n2 = arcgraph::latticeImplied(L, t.cycle, &new2);
        if (!n2 || new2 >= ep) continue;
        Unit u;
        u.l1 = a;
        u.l2 = b;
        u.n1 = *n1;
        u.n2 = *n2;
        out.push_back(u);
    }
    return out;
}

std::vector<ArLatticeReleaseEvent> localizeFailures(const std::vector<Unit>& units, const ArEpochFixes& fixes) {
    // How many failing units run through each band-arc.
    std::map<ArcId, int> failing_through;
    for (const Unit& u : units) {
        if (!u.fail) continue;
        std::vector<ArcId> arcs = unitArcs(u, fixes);
        std::sort(arcs.begin(), arcs.end());
        arcs.erase(std::unique(arcs.begin(), arcs.end()), arcs.end());
        for (const ArcId& a : arcs) ++failing_through[a];
    }
    std::vector<ArLatticeReleaseEvent> out;
    std::set<ArcId> shared;
    for (const auto& [arc, n] : failing_through) {
        if (n < 2) continue;
        shared.insert(arc);
        ArLatticeReleaseEvent r;
        r.cause = ArLatticeRelease::Revalidation;
        r.arc   = arc;
        out.push_back(r);
    }
    for (const Unit& u : units) {
        if (!u.fail) continue;
        const std::vector<ArcId> arcs = unitArcs(u, fixes);
        if (std::any_of(arcs.begin(), arcs.end(), [&](const ArcId& a) { return shared.count(a) > 0; }))
            continue;   // the shared arc explains it
        for (int k : {u.l1, u.l2}) {
            const SearchedCycle& s = fixes.floats[static_cast<std::size_t>(k)];
            ArLatticeReleaseEvent r;
            r.cause = ArLatticeRelease::Revalidation;
            r.scope = ArLatticeScope::Cycle;
            r.cycle = s.cycle;
            r.dd    = s.id;
            out.push_back(r);
        }
    }
    return out;
}

void markReleased(std::vector<Unit>& units, const arcgraph::CycleLattice& L, const ArEpochFixes& fixes) {
    for (Unit& u : units) {
        if (u.fail || u.released) continue;
        const std::optional<long long> n1 = arcgraph::latticeImplied(L, fixes.floats[static_cast<std::size_t>(u.l1)].cycle);
        const std::optional<long long> n2 = arcgraph::latticeImplied(L, fixes.floats[static_cast<std::size_t>(u.l2)].cycle);
        u.released = !(n1 && n2 && *n1 == u.n1 && *n2 == u.n2);
    }
}

std::vector<Unit> revalidateUnits(ArLatticeState& S, const ArEpochFixes& fixes, long long ep, double alpha,
                                  ArLatticeEpoch& le) {
    std::vector<Unit> units = impliedUnits(S.lattice, fixes, ep);
    scoreUnits(units, fixes, alpha);
    le.units        = static_cast<int>(units.size());
    le.units_failed = static_cast<int>(std::count_if(units.begin(), units.end(), [](const Unit& u) { return u.fail; }));
    arLatticeRelease(S, localizeFailures(units, fixes), le);
    markReleased(units, S.lattice, fixes);
    return units;
}

void admitFixes(ArLatticeState& S, const ArEpochFixes& fixes, int persist_ep, long long ep, ArLatticeEpoch& le,
                std::vector<Unit>* ledger_units) {
    arLatticeAdmit(S, fixes, persist_ep, ep, le);
    if (ledger_units) markReleased(*ledger_units, S.lattice, fixes);
}

void markLapsed(std::vector<Unit>& units, const ArEpochFixes& fixes, std::map<ArDdIdentity, int>& unkept,
                int lapse_ep) {
    if (lapse_ep <= 0) return;
    std::map<ArDdIdentity, int> now;
    for (Unit& u : units) {
        const ArDdIdentity& id = fixes.floats[static_cast<std::size_t>(u.l1)].id;
        int n = 0;
        if (!searchKept(u, fixes)) {
            const auto it = unkept.find(id);
            n = (it == unkept.end() ? 0 : it->second) + 1;
        }
        now[id]  = n;
        u.lapsed = n >= lapse_ep;
    }
    unkept = std::move(now);
}

double chi2Quantile(int dof, double p) {
    if (dof < 1 || !(p > 0.0 && p < 1.0))
        throw std::invalid_argument("fcyc::chi2Quantile: dof must be >= 1 and p in (0, 1)");
    if (dof == 2) return -2.0 * std::log(1.0 - p);
    const double k = static_cast<double>(dof);
    double lo = 0.0, hi = k + 50.0 * std::sqrt(2.0 * k) + 100.0;
    while (gammaP(0.5 * k, 0.5 * hi) < p) hi *= 2.0;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (gammaP(0.5 * k, 0.5 * mid) < p) lo = mid; else hi = mid;
        if (hi - lo <= 1e-12 * hi) break;
    }
    return 0.5 * (lo + hi);
}

FoldPlan placeUnits(const std::vector<Unit>& units, const std::vector<char>& active,
                    const ArEpochFixes& fixes, double sigma_cyc) {
    FoldPlan plan;
    // Each active unit's two rows, or unplaced when a band-arc has no live column.
    struct Placed { int unit; NetworkNormalEquations::CycleRow r1, r2; std::vector<int> stations; };
    std::vector<Placed> placed;
    auto rowOf = [&](const SearchedCycle& s, long long n, NetworkNormalEquations::CycleRow& row,
                     std::vector<int>& sta) {
        const double lambda = lambdaOf(s);
        double lin = 0.0;
        for (const arcgraph::CycleTerm& t : s.cycle.terms) {
            const auto it = fixes.columns.find(t.arc);
            if (it == fixes.columns.end() || it->second.local_col < 0) return false;
            const ArcColumn& c = it->second;
            row.terms.push_back({c.est_station, static_cast<Eigen::Index>(c.local_col),
                                 static_cast<double>(t.coeff)});
            lin += static_cast<double>(t.coeff) * c.x_lin_m;
            sta.push_back(static_cast<int>(c.est_station));
        }
        row.zhat_m = lambda * static_cast<double>(n) - lin;
        row.pz_m2  = (sigma_cyc * lambda) * (sigma_cyc * lambda);
        return !row.terms.empty();
    };
    for (std::size_t u = 0; u < units.size(); ++u) {
        if (u >= active.size() || active[u] == 0) continue;
        Placed p;
        p.unit = static_cast<int>(u);
        const bool ok1 = rowOf(fixes.floats[static_cast<std::size_t>(units[u].l1)], units[u].n1, p.r1, p.stations);
        const bool ok2 = ok1 && rowOf(fixes.floats[static_cast<std::size_t>(units[u].l2)], units[u].n2, p.r2, p.stations);
        if (!ok2) { plan.unplaced.push_back(p.unit); continue; }
        placed.push_back(std::move(p));
    }
    // One block per connected station component (union-find over estimator stations), ordered by
    // the component's first unit.
    arcgraph::Dsu dsu;
    for (const Placed& p : placed)
        for (std::size_t k = 1; k < p.stations.size(); ++k) arcgraph::dsuUnite(dsu, p.stations[k], p.stations[0]);
    std::map<int, int> block_of_root;
    for (Placed& p : placed) {
        const int root = arcgraph::dsuFind(dsu, p.stations.front());
        const auto [it, fresh] = block_of_root.emplace(root, static_cast<int>(plan.blocks.size()));
        if (fresh) {
            plan.blocks.emplace_back();
            plan.block_units.emplace_back();
        }
        const std::size_t b = static_cast<std::size_t>(it->second);
        plan.blocks[b].push_back(std::move(p.r1));
        plan.blocks[b].push_back(std::move(p.r2));
        plan.block_units[b].push_back(p.unit);
    }
    return plan;
}

EpochFold foldUnits(const NetworkEstimator& est, const NetworkEstimator::Datum& datum,
                    const Eigen::VectorXd& x_s, const std::vector<Unit>& units,
                    const ArEpochFixes& fixes, const FoldKnobs& k) {
    EpochFold f;
    f.fate.resize(units.size());
    std::vector<char> active(units.size(), 0);
    for (std::size_t u = 0; u < units.size(); ++u) {
        if (units[u].fail)          f.fate[u].action = UnitAction::UnitRefused;
        else if (units[u].released) f.fate[u].action = UnitAction::Released;
        else if (units[u].lapsed)   f.fate[u].action = UnitAction::Lapsed;
        else                        active[u] = 1;
    }
    auto settle = [&](int u, UnitAction a, int block, int pass, const BlockFigures& fig) {
        UnitFate& fa = f.fate[static_cast<std::size_t>(u)];
        fa.action = a;
        fa.block  = block;
        fa.pass   = pass;
        fa.fig    = fig;
        active[static_cast<std::size_t>(u)] = 0;
    };
    // Every pass refolds the units still active.  Up to max_iter passes drop each failing
    // block's worst unit; the pass after that drops what still fails whole and folds the rest
    // once more.  The bound is a safeguard: a block's own statistic depends on its rows alone,
    // so the last refold does not fail again.
    const int last_pass = k.max_iter + 1;
    for (int pass = 0; pass <= last_pass + 1; ++pass) {
        f.plan = placeUnits(units, active, fixes, k.sigma_cyc);
        for (int u : f.plan.unplaced) settle(u, UnitAction::Unplaced, -1, pass, {});
        f.blocks.assign(f.plan.blocks.size(), BlockFigures{});
        if (f.plan.blocks.empty()) return f;
        try {
            f.held = est.solveHeldDetached(datum, x_s, f.plan.blocks);
        } catch (const std::exception& e) {
            f.error = e.what();
            for (std::size_t b = 0; b < f.plan.block_units.size(); ++b)
                for (int u : f.plan.block_units[b]) settle(u, UnitAction::KernelError, static_cast<int>(b), pass, {});
            ++f.passes;
            return f;
        }
        ++f.passes;
        // The kernel's own refusals end the epoch float (the held estimate is the float, bitwise).
        bool any_folded = false;
        for (std::size_t b = 0; b < f.plan.blocks.size(); ++b) {
            if (f.held.degenerate[b]) {
                for (int u : f.plan.block_units[b]) settle(u, UnitAction::Degenerate, static_cast<int>(b), pass, {});
                continue;
            }
            any_folded = true;
            BlockFigures& fig = f.blocks[b];
            fig.k           = static_cast<int>(f.plan.blocks[b].size());
            fig.chi2        = f.held.chi2[b];
            fig.chi2_expect = f.held.chi2_expect[b];
            fig.bar         = chi2Quantile(fig.k, 1.0 - k.gate_alpha);
        }
        if (!any_folded) return f;
        if (!f.held.ok) {
            const UnitAction a = (f.held.float_repro_rel > NetworkEstimator::kHeldFloatReproTol)
                                     ? UnitAction::FloatMismatch : UnitAction::NonFinite;
            for (std::size_t b = 0; b < f.plan.blocks.size(); ++b)
                if (!f.held.degenerate[b])
                    for (int u : f.plan.block_units[b]) settle(u, a, static_cast<int>(b), pass, f.blocks[b]);
            return f;
        }
        // Acceptance: each block's own statistic against its bar.
        std::vector<std::size_t> failing;
        for (std::size_t b = 0; b < f.plan.blocks.size(); ++b)
            if (!f.held.degenerate[b] && !(f.blocks[b].chi2 <= f.blocks[b].bar)) failing.push_back(b);
        if (failing.empty()) {
            if (!(f.held.max_shift_m <= k.max_shift_m)) {
                for (std::size_t b = 0; b < f.plan.blocks.size(); ++b)
                    if (!f.held.degenerate[b])
                        for (int u : f.plan.block_units[b]) settle(u, UnitAction::ShiftRefused, static_cast<int>(b), pass, f.blocks[b]);
                return f;
            }
            for (std::size_t b = 0; b < f.plan.blocks.size(); ++b)
                if (!f.held.degenerate[b])
                    for (int u : f.plan.block_units[b]) settle(u, UnitAction::Folded, static_cast<int>(b), pass, f.blocks[b]);
            f.accepted = true;
            return f;
        }
        f.failures += static_cast<int>(failing.size());
        for (std::size_t b : failing) {
            const std::vector<int>& bu = f.plan.block_units[b];
            if (pass < k.max_iter) {
                // Drop the block's unit of largest T (the per-unit statistic ranks them).
                int worst = bu.front();
                for (int u : bu)
                    if (units[static_cast<std::size_t>(u)].t > units[static_cast<std::size_t>(worst)].t) worst = u;
                settle(worst, UnitAction::Dropped, static_cast<int>(b), pass, f.blocks[b]);
            } else {
                for (int u : bu) settle(u, UnitAction::LeftFloat, static_cast<int>(b), pass, f.blocks[b]);
            }
        }
    }
    // Not reached while a block's statistic depends on its rows alone; leave the epoch float.
    f.accepted = false;
    for (std::size_t u = 0; u < units.size(); ++u)
        if (active[u]) settle(static_cast<int>(u), UnitAction::LeftFloat, -1, last_pass + 1, {});
    return f;
}

std::vector<ArLatticeReleaseEvent> foldReleases(const std::vector<Unit>& units, const EpochFold& f,
                                                const ArEpochFixes& fixes) {
    std::vector<ArLatticeReleaseEvent> out;
    for (std::size_t u = 0; u < units.size() && u < f.fate.size(); ++u) {
        const UnitAction a = f.fate[u].action;
        if (a != UnitAction::Dropped && a != UnitAction::Degenerate) continue;
        for (int k : {units[u].l1, units[u].l2}) {
            const SearchedCycle& s = fixes.floats[static_cast<std::size_t>(k)];
            ArLatticeReleaseEvent r;
            r.cause = a == UnitAction::Dropped ? ArLatticeRelease::Acceptance : ArLatticeRelease::Degenerate;
            r.scope = ArLatticeScope::Cycle;
            r.cycle = s.cycle;
            r.dd    = s.id;
            out.push_back(r);
        }
    }
    return out;
}

}  // namespace fcyc

FixedCycleEmission networkFixedCycles(NetworkRun& run, const NetworkEpoch& nep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    // The frozen body declares or uses `ep`, so this definition names the parameter nep
    // (specification 3.2).
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = nep.se;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = nep.solve.x_s;
    const NetworkEstimator::Datum& datum = nep.solve.plan.shared_datum;
    const ArEpochFixes& fixes = nep.ar_fixes;
    const WithinEpochCulprits& we = nep.solve.we_landed;
    const std::vector<NetStation>& stations = run.setup.stations;
    const EpochEvents& events = nep.events;
    // ---- End of the bindings ----
    const auto& ar = config_.getNetworkConfig().ambiguity_resolution;
    const bool fold   = ar.fixed_cycle_fold != FixedCycleFold::Off;
    const bool ledger = arLatticeArmed(config_);
    if (!fold && !ledger) return {};
    FixedCycleCounters& C = ctx.counters.fixed_cycles;
    if (fold) ++C.epochs;
    if (!fixes.searched) {   // no search, no step: the ledger's next step releases everything
        if (ledger) ++ctx.counters.netar_lattice.epochs_skipped;
        return {};
    }
    if (fold) ++C.epochs_searched;
    const bool from_lattice = fold && ar.fixed_cycle_source == FixedCycleSource::Lattice;
    const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const auto t0 = std::chrono::steady_clock::now();
    double ledger_ms = 0.0;
    auto timed = [&ledger_ms](auto&& step) {
        const auto t = std::chrono::steady_clock::now();
        step();
        ledger_ms += msSince(t);
    };
    ArLatticeState& S = ctx.carry.ar_lattice;

    // 1. The ledger opens: the seam gap, the evidence releases, the contradictions.
    ArLatticeEpoch le;
    if (ledger) timed([&] { le = arLatticeBegin(S, fixes, networkArLatticeEvidence(ctx, we, events), ep); });

    // 2-3. The units: this epoch's determined pairs, or the ledger's after a planted integer and
    //      the re-validation (its failures released).
    std::vector<fcyc::Unit> units;
    if (from_lattice) {
        timed([&] {
            plantFixedCycles(ctx, log, traces, se, stations, S.lattice);
            units = fcyc::revalidateUnits(S, fixes, ep, ar.fixed_cycle_unit_alpha, le);
        });
        C.implied_per_epoch.push_back(static_cast<double>(units.size()));
    } else if (fold) {
        units = fcyc::epochUnits(fixes);
        fcyc::scoreUnits(units, fixes, ar.fixed_cycle_unit_alpha);
    }

    // 4. Admission: the fixes that persisted enter the ledger (they fold from the next epoch); a
    //    unit an admission contradiction took is marked released.
    if (ledger)
        timed([&] { fcyc::admitFixes(S, fixes, ar.fixed_cycle_persist_ep, ep, le, from_lattice ? &units : nullptr); });

    // 5. The lapse (source lattice): a unit the search no longer determines stops folding.
    if (from_lattice) fcyc::markLapsed(units, fixes, ctx.carry.fixed_cycles.unkept, ar.fixed_cycle_lapse_ep);

    // 6. The fold, with its acceptance; under source lattice its refusals release their arcs.
    FixedCycleEmission out;
    fcyc::EpochFold f;
    if (fold && !units.empty()) {
        ++C.epochs_with_pair;
        f = fcyc::foldUnits(est, datum, x_s, units, fixes, foldKnobsOf(ar));
        if (from_lattice) timed([&] { arLatticeRelease(S, fcyc::foldReleases(units, f, fixes), le); });
        tallyFold(C, units, f, fixes);
        if (!f.error.empty())
            log->warn("Network fixed-cycle fold (card FCF): TOW {:.1f}: the held solve threw ({}); the "
                      "epoch stays float.", se.time.tow, f.error);
        if (traces.fixed_cycles_trace.is_open())
            writeUnitRows(traces.fixed_cycles_trace, se, stations, fixes, units, f);
        if (f.accepted && traces.fixed_cycle_shift_trace.is_open())
            writeShiftRows(traces.fixed_cycle_shift_trace, se, est, x_s, f.held.x_s);
        C.seam_ms.push_back(msSince(t0));
        if (ar.fixed_cycle_fold == FixedCycleFold::Emit && f.accepted) {
            ++C.emitted;
            out = FixedCycleEmission{true, f.held.x_s};
        }
    }

    // 7. The fold's edges (source lattice).
    if (from_lattice) foldEdges(ctx.carry.fixed_cycles, C, traces, se, stations, fixes, units, f, le, ep);

    // 8. The ledger's record.
    if (ledger) {
        arLatticeClose(S, ep, le);
        networkArLatticeRecord(ctx, log, traces, se, stations, fixes, le, ledger_ms);
    }
    return out;
}

// ---- Run-end reports (card NOM step TD1b) ----

// Run-end report: the fixed-cycle fold (card FCF).
int networkFixedCyclesRunEnd(const NetworkRunEndView& v) {
    const auto& ar = v.config.getNetworkConfig().ambiguity_resolution;
    if (ar.fixed_cycle_fold == FixedCycleFold::Off) return 0;
    const FixedCycleCounters& C = v.ctx.counters.fixed_cycles;
    using fcyc::UnitAction;
    const bool emit    = ar.fixed_cycle_fold == FixedCycleFold::Emit;
    const bool lattice = ar.fixed_cycle_source == FixedCycleSource::Lattice;
    const double unit_bar = -2.0 * std::log(ar.fixed_cycle_unit_alpha);
    long long t_above = 0;
    for (double t : C.unit_t) if (!(t <= unit_bar)) ++t_above;
    const auto act = [&](UnitAction a) { return C.by_action[static_cast<std::size_t>(a)]; };

    v.log->info("Network (fixed-cycle fold, card FCF): mode {}, source {}, hold {} cycle.  {} "
                "epoch(s) armed, {} with the search, {} with a determined L1/L2 pair; {} held "
                "solve(s) accepted ({} pass(es) run){}.  {} unit(s): [{}].  Folded units per "
                "accepted epoch p50 {:.0f}, max {:.0f}; {} block(s) folded (K p50 {:.0f}, max {:.0f}).",
                emit ? "EMIT" : "observe", lattice ? "lattice" : "epoch", ar.fixed_cycle_sigma_cyc,
                C.epochs, C.epochs_searched,
                C.epochs_with_pair, C.held_solves, C.passes,
                emit ? fmt::format(", {} handed to the emission", C.emitted) : std::string(),
                C.units, actionText(C.by_action), RobustStats::median(C.units_per_epoch),
                maxOf(C.units_per_epoch), C.blocks_folded, RobustStats::median(C.block_k),
                maxOf(C.block_k));
    v.log->info("Network (fixed-cycle fold, card FCF): unit test T (chi-square, 2 dof) p50 {:.3f}, "
                "p95 {:.3f}, max {:.3f}; {} of {} above the bar {:.2f} (alpha {:.0e}, rate {:.2e}).  "
                "Block acceptance: own statistic over its stated mean p50 {:.3f}, p95 {:.3f}; "
                "stated mean over K p50 {:.3f}; {} block failure(s) (dropped {}, left float {}).",
                quantileOf(C.unit_t, 0.5), quantileOf(C.unit_t, 0.95), maxOf(C.unit_t), t_above,
                C.unit_t.size(), unit_bar, ar.fixed_cycle_unit_alpha,
                C.unit_t.empty() ? 0.0 : static_cast<double>(t_above) / static_cast<double>(C.unit_t.size()),
                quantileOf(C.chi2_ratio, 0.5), quantileOf(C.chi2_ratio, 0.95),
                quantileOf(C.expect_per_dof, 0.5), C.blocks_failed, act(UnitAction::Dropped),
                act(UnitAction::LeftFloat));
    v.log->info("Network (fixed-cycle fold, card FCF): guards: float_mismatch {} unit(s), "
                "non_finite {}, kernel_errors {}, shift_refused {}, degenerate block(s) {} ({} "
                "dependent), epochs with a pair and no accepted held solve {}.  Float reproduction "
                "(normwise backward error) p50 {:.2e}, p95 {:.2e}, max {:.2e} (bar {:.0e}); largest "
                "shift held minus float p50 {:.4f} m, max {:.4f} m (bound {} m); factor nonzeros at "
                "most {}.  Seam cost per epoch with a pair: p50 {:.2f} ms, p95 {:.2f} ms, max {:.2f} ms.",
                act(UnitAction::FloatMismatch), act(UnitAction::NonFinite), act(UnitAction::KernelError),
                act(UnitAction::ShiftRefused), C.blocks_degenerate, C.blocks_dependent, C.epochs_refused,
                quantileOf(C.repro, 0.5), quantileOf(C.repro, 0.95), maxOf(C.repro),
                NetworkEstimator::kHeldFloatReproTol, quantileOf(C.shift_m, 0.5), maxOf(C.shift_m),
                ar.fixed_cycle_max_shift_m, C.factor_nnz_max, quantileOf(C.seam_ms, 0.5),
                quantileOf(C.seam_ms, 0.95), maxOf(C.seam_ms));
    if (lattice) {
        const ArLatticeCounters& L = v.ctx.counters.netar_lattice;
        v.log->info("Network (fixed-cycle fold, card FCF): the ledger's units (admitted after {} "
                    "epoch(s) at one integer): implied per searched epoch p50 {:.0f}, max {:.0f}; "
                    "{} implied, {} failing their float test and released (the ledger's report "
                    "names the rows), {} released with another unit's rows, {} lapsed (lapse_ep "
                    "{}{}); {} folded unit-epoch(s) whose pair the search did not determine at "
                    "that epoch (held by the ledger alone); folded runs: {} onset(s), {} "
                    "offset(s); {} late post-fit flag(s) on an arc of a unit folded at the step "
                    "before (RAIM T3 / T4, the gross-LOS Act); {} planted integer(s).",
                    ar.fixed_cycle_persist_ep, RobustStats::median(C.implied_per_epoch),
                    maxOf(C.implied_per_epoch), L.units, L.units_failed, act(UnitAction::Released),
                    act(UnitAction::Lapsed), ar.fixed_cycle_lapse_ep,
                    ar.fixed_cycle_lapse_ep > 0 ? "" : ", off", C.folded_unkept, C.onsets, C.offsets,
                    C.late_t3t4, C.plants);
        if (L.units == 0)
            v.log->warn("Network (fixed-cycle fold, card FCF): source lattice ARMED, but the ledger "
                        "never implied a unit ({} step(s)); every fold number above is structurally "
                        "empty (standing invariant 4).", L.epochs);
    }
    if (C.held_solves != C.epochs_with_pair)
        v.log->warn("Network (fixed-cycle fold, card FCF): {} epoch(s) had a determined L1/L2 pair "
                    "but only {} held solve(s) were accepted (standing invariant 4): {} epoch(s) "
                    "stayed float; their units' fates are counted above and traced.",
                    C.epochs_with_pair, C.held_solves, C.epochs_with_pair - C.held_solves);
    if (C.held_solves == 0)
        v.log->warn("Network (fixed-cycle fold, card FCF): ARMED, but no held solve was ever "
                    "accepted ({} epoch(s) with a pair); every fold number above is structurally "
                    "empty (standing invariant 4).", C.epochs_with_pair);
    if (act(UnitAction::KernelError) + act(UnitAction::FloatMismatch) + act(UnitAction::NonFinite) > 0
        || C.blocks_degenerate > 0)
        v.log->warn("Network (fixed-cycle fold, card FCF): the kernel refused or failed: {} unit(s) "
                    "by kernel error, {} by float mismatch, {} by a non-finite held estimate, {} "
                    "degenerate block(s); each such epoch stayed float.",
                    act(UnitAction::KernelError), act(UnitAction::FloatMismatch),
                    act(UnitAction::NonFinite), C.blocks_degenerate);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
