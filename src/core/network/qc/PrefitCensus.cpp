/**
 * @file PrefitCensus.cpp
 * @brief Implementation of the prefit census / structural fuse / seed-birth
 *        tripwire / topology ledger (see PrefitCensus.hpp).
 */

#include "network/qc/PrefitCensus.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "filter/StateManager.hpp"
#include "network/diag/RunTraces.hpp"

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/driver/RunTypes.hpp"

#include "network/driver/Epoch.hpp"        // NetworkEpoch (the solve stages)
#include "network/driver/Run.hpp"          // NetworkRun
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum, NetworkPostFitHandoff
#include "network/diag/RunTraces.hpp"

namespace GnssProcessor {

namespace {

/// Per-epoch cap on shared-column add/remove events, each direction.  A cold
/// start registers hundreds of columns at once; the ledger is for steady-state
/// changes, not a transcript of the first epochs.  Truncated events are counted
/// (@c topo_truncated, reported at run end) and the epoch is logged
/// (invariant #4).
constexpr int kTopoColEventCap = 64;

/// One classified row: its class and the satellite its contribution tag names.
/// (Named apart from the declared contribution tag, RowTag of
/// network/assembly/Contributions.hpp.)
struct ClassifiedRow {
    NetRowClass cls{NetRowClass::ObsOther};
    std::string sat;
};

/// Classify every row of one station once, from the rows' tags.
void classifyStation(const StationAssembly& R, std::vector<ClassifiedRow>& tags) {
    const Eigen::Index m = R.y.size();
    tags.assign(static_cast<std::size_t>(std::max<Eigen::Index>(m, 0)), ClassifiedRow{});
    for (Eigen::Index row = 0; row < m; ++row) {
        ClassifiedRow& tg = tags[static_cast<std::size_t>(row)];
        tg.cls = classifyRow(R, row, &tg.sat);
    }
}

/// Robust per-type centres from a pre-classified station (the shared core of
/// @ref prefitStationCentres and of both walks).
PrefitStationCentres centresFrom(const StationAssembly& R,
                                 const std::vector<ClassifiedRow>& tags) {
    PrefitStationCentres out;
    std::vector<double> pool[kNetDataClasses];
    const Eigen::Index m = std::min<Eigen::Index>(
        R.y.size(), static_cast<Eigen::Index>(tags.size()));
    for (Eigen::Index row = 0; row < m; ++row) {
        const int t = dataClassIndex(tags[static_cast<std::size_t>(row)].cls);
        if (t < 0) continue;
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const double y = R.y(row);
        if (!std::isfinite(y) || !std::isfinite(w)) continue;
        ++out.n[t];
        // iono_sh_row carries no receiver clock, so it is not centred (m ≡ 0).
        if (t != static_cast<int>(NetRowClass::IonoShRow))
            pool[t].push_back(y);
    }
    for (int t = 0; t < kNetDataClasses; ++t) {
        if (t == static_cast<int>(NetRowClass::IonoShRow)) {
            out.m[t] = 0.0;
            out.have[t] = out.n[t] > 0;
            continue;
        }
        if (out.n[t] >= kNetPrefitMinTypeRows) {
            std::vector<double>& p = pool[t];
            const std::size_t k = p.size() / 2;
            std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(k), p.end());
            out.m[t] = p[k];
            out.have[t] = true;
        }
    }
    return out;
}

/// The worst row of one (satellite, class) cell at one station this epoch.
struct CellWorst {
    double       s{0.0};
    double       y{0.0};
    double       w{0.0};
    double       m{0.0};
    Eigen::Index row{-1};
};

/// Rate-limited event gate: true when this (event, station, sat, class) key may
/// emit again, i.e. no earlier emission or at least @p holdoff loop epochs since
/// the last one (holdoff <= 0 disables the limit).  A parked cell keeps logging
/// its identity, so its duration is readable from the events, but not once per
/// epoch.
bool holdoffAllows(NetworkRunContext& ctx, const char* event, const std::string& station,
                   const std::string& sat, const char* cls, long long seq, long long holdoff) {
    net::LosTable& los_rows = ctx.entities.los;   // written
    // The line of sight's clock of (event, class): the station by NAME (O3), and "?" (the
    // census's no-satellite placeholder) is the station's own row, SatId{}.
    const std::optional<LosId> id =
        net::losOfName(ctx.entities, station, sat == "?" ? std::string() : sat);
    const std::string sub = std::string(event) + "|" + cls;
    const long long* last =
        id ? net::get(ctx.entities.los, *id, &net::LosState::events, sub,
                      &net::LosEventState::prefit_event_last)
           : nullptr;
    if (last != nullptr && holdoff > 0 && (seq - *last) < holdoff) return false;
    if (id)
        net::put(los_rows, *id, &net::LosState::events, sub,
                 &net::LosEventState::prefit_event_last) = seq;
    return true;
}

/// "code+phase" style rendering of a row-kind bitmask (INC-4 event detail).
std::string kindMaskName(unsigned mask) {
    std::string s;
    for (int t = 0; t < kNetDataClasses; ++t) {
        if ((mask & (1u << t)) == 0) continue;
        if (!s.empty()) s += "+";
        s += rowClassName(static_cast<NetRowClass>(t));
    }
    return s.empty() ? "none" : s;
}

} // namespace

unsigned parseDataClassMask(const std::string& spec, std::vector<std::string>* unknown) {
    unsigned mask = 0;
    std::string tok;
    auto flush = [&] {
        if (tok.empty()) return;
        for (char& ch : tok) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if      (tok == "code")  mask |= dataClassBit(NetRowClass::ObsCode);
        else if (tok == "phase") mask |= dataClassBit(NetRowClass::ObsPhase);
        else if (tok == "other") mask |= dataClassBit(NetRowClass::ObsOther);
        else if (tok == "sh" || tok == "iono_sh_row" || tok == "iono")
                                 mask |= dataClassBit(NetRowClass::IonoShRow);
        else if (tok == "all")   mask |= (1u << kNetDataClasses) - 1u;
        else if (tok == "none")  { /* explicit empty domain */ }
        else if (unknown)        unknown->push_back(tok);
        tok.clear();
    };
    for (char ch : spec) {
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == '+' || ch == '|') flush();
        else tok.push_back(ch);
    }
    flush();
    return mask;
}

std::string dataClassMaskName(unsigned mask) {
    std::string s;
    for (int t = 0; t < kNetDataClasses; ++t) {
        if ((mask & (1u << t)) == 0) continue;
        if (!s.empty()) s += "+";
        s += rowClassName(static_cast<NetRowClass>(t));
    }
    return s.empty() ? "none" : s;
}

double prefitStandardizedDeviation(double y, double w, double m) {
    if (!std::isfinite(y) || !std::isfinite(w) || !std::isfinite(m)) return 0.0;
    const double s = std::abs(y - m) * std::sqrt(std::max(0.0, w));
    return std::isfinite(s) ? s : 0.0;
}

PrefitStationCentres prefitStationCentres(const StationAssembly& R) {
    std::vector<ClassifiedRow> tags;
    classifyStation(R, tags);
    return centresFrom(R, tags);
}

// =====================================================================
//  H-rider: the pooled robust prefit σ̂₀ for the H heartbeat
// =====================================================================

double networkPrefitSigma0(const NetworkRunContext& ctx,
                           const std::vector<NetStation>& stations,
                           const NetworkEstimator& est,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           unsigned class_mask) {
    const Eigen::Index n = est.numShared();
    if (n <= 0 || class_mask == 0) return 0.0;

    std::vector<double> pool;
    const bool have_prior = ctx.carry.prefit_walk_seeded;
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;

        std::vector<ClassifiedRow> tags;
        classifyStation(R, tags);
        const PrefitStationCentres c = centresFrom(R, tags);

        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            const ClassifiedRow& tg = tags[static_cast<std::size_t>(row)];
            const int ti = dataClassIndex(tg.cls);
            if (ti < 0) continue;                              // pins / coupling
            if (!(class_mask & dataClassBit(tg.cls))) continue;
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double y = R.y(row);
            if (!std::isfinite(y) || !std::isfinite(w)) continue;
            if (w >= kNetPrefitPinWeight) continue;            // pin-regime DATA row
            if (!c.have[ti]) continue;                         // no usable centre
            // Skip cells born this epoch (as INC-1 does).  An SH tie row is a structural
            // row, not a per-cell birth, so it is never deferred, although its tag names
            // its line of sight's satellite (the row classifier keeps that rule).
            if (tg.cls != NetRowClass::IonoShRow && !tg.sat.empty() && have_prior
                && !net::hasLos(ctx.entities.los, static_cast<int>(i), tg.sat,
                                &net::LosState::prefit_present_prev))
                continue;
            pool.push_back(prefitStandardizedDeviation(y, w, c.m[ti]));
        }
    }
    if (pool.empty()) return 0.0;   // the H grammar's "unavailable" sentinel
    const std::size_t mid = pool.size() / 2;
    std::nth_element(pool.begin(), pool.begin() + mid, pool.end());
    double med = pool[mid];
    if (pool.size() % 2 == 0) {
        std::nth_element(pool.begin(), pool.begin() + (mid - 1), pool.end());
        med = 0.5 * (pool[mid - 1] + med);
    }
    return std::isfinite(med) ? 1.4826 * med : 0.0;
}

// =====================================================================
//  Structural fuse (INC-2)
// =====================================================================

int networkPrefitFuseStation(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, Eigen::Index n_shared,
                             const std::string& station, StationAssembly& R,
                             double s_fuse) {
    if (s_fuse <= 0.0 || !R.ok || R.y.size() == 0 || R.A_s.cols() != n_shared) return 0;
    auto& ctr = ctx.counters.pc;
    ++ctr.fuse_station_epochs;

    std::vector<ClassifiedRow> tags;
    classifyStation(R, tags);
    const PrefitStationCentres c = centresFrom(R, tags);

    const long long seq = static_cast<long long>(ctx.counters.n_loop_epochs);
    const long long holdoff = ctx.knobs.pc_event_holdoff;
    std::map<std::pair<std::string, int>, CellWorst> worst;   // pre-cap identity
    int capped = 0;

    for (Eigen::Index row = 0; row < R.y.size(); ++row) {
        const ClassifiedRow& tg = tags[static_cast<std::size_t>(row)];
        const int ti = dataClassIndex(tg.cls);
        if (ti < 0) continue;                       // pins / coupling: never fused
        ++ctr.fuse_rows;
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const double y = R.y(row);
        if (!std::isfinite(y) || !std::isfinite(w) || w <= 0.0) continue;
        if (w >= kNetPrefitPinWeight) { ++ctr.fuse_skip_pin_w; continue; }
        if (!c.have[ti])              { ++ctr.fuse_skip_thin;  continue; }

        const double s = prefitStandardizedDeviation(y, w, c.m[ti]);
        // Domain restriction (network.prefit_fuse_classes), counted two ways:
        // `fuse_skip_class` is the exposure and `fuse_would_class` the caps given
        // up, so the cost of narrowing the domain is measured.  The knob's doc
        // explains why phase is out by default.
        if ((ctx.knobs.pc_fuse_mask & (1u << static_cast<unsigned>(ti))) == 0) {
            ++ctr.fuse_skip_class;
            if (s > s_fuse) ++ctr.fuse_would_class;
            continue;
        }
        if (s <= s_fuse) continue;

        // Continuous cap: the row contributes at most an S_fuse-equivalent
        // standardized load.  The row is kept (a cap, not a drop): the cap is
        // order-preserving and smooth, and the row keeps its sign.
        const double ratio = (s_fuse / s) * (s_fuse / s);
        R.w(row) = w * ratio;
        ++capped;
        ++ctr.fuse_capped;
        if (s > ctr.fuse_max_s) ctr.fuse_max_s = s;
        if (ratio < ctr.fuse_min_ratio) ctr.fuse_min_ratio = ratio;

        const std::string sat = tg.sat.empty() ? std::string("?") : tg.sat;
        CellWorst& cw = worst[{sat, ti}];
        if (s > cw.s) cw = CellWorst{s, y, w, c.m[ti], row};
    }

    for (const auto& [key, cw] : worst) {
        const char* cls = rowClassName(static_cast<NetRowClass>(key.second));
        if (!holdoffAllows(ctx, "fused", station, key.first, cls, seq, holdoff)) {
            ++ctr.fuse_events_held;
            continue;
        }
        ++ctr.fuse_events;
        traces.emitEvent(t, "PREFIT", "prefit_fused", key.first, station, cw.s, s_fuse,
                         "cap_weight",
                         fmt::format("class={} row={} y={:.6g} m={:.6g} w={:.6g} "
                                     "w_eff={:.6g}",
                                     cls, cw.row, cw.y, cw.m, cw.w,
                                     cw.w * (s_fuse / cw.s) * (s_fuse / cw.s)));
    }
    return capped;
}

// =====================================================================
//  Post-solve walk: census, seed-birth tripwire, topology ledger (INC-1 / INC-3 / INC-4)
// =====================================================================

void networkPrefitCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         const NetworkEstimator& est,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of, EpochEvents& events) {
    net::NetworkEntities& entities = ctx.entities;   // written: los stations
    if (!ctx.knobs.pc_walk_on) return;
    (void)config_;   // arming is expressed entirely through the knob snapshot
    // The census scalars are epoch events: an epoch the walk cannot serve leaves the
    // epoch-status columns blank instead of repeating the previous epoch's numbers.
    EpochEvents::Census& census_ev = events.census;

    const bool   census_on   = ctx.knobs.pc_census_on;
    const bool   topo_on     = ctx.knobs.pc_topo_on;
    const double ev_sigma    = ctx.knobs.pc_event_sigma;
    const double birth_sigma = ctx.knobs.pc_birth_sigma;
    const long long holdoff  = ctx.knobs.pc_event_holdoff;
    const long long seq      = static_cast<long long>(ctx.counters.n_loop_epochs);
    auto& ctr = ctx.counters.pc;

    const Eigen::Index n = est.numShared();
    if (n <= 0) return;
    const SharedColumnMap cm = buildSharedColumnMap(est, n);
    ++ctr.epochs;

    // Per-epoch census accumulators (DATA classes only).
    std::vector<double> s_pool[kNetDataClasses];
    double    max_s[kNetDataClasses]    = {0.0, 0.0, 0.0, 0.0};
    long long n_gt_10[kNetDataClasses]  = {0, 0, 0, 0};
    long long n_gt_1k[kNetDataClasses]  = {0, 0, 0, 0};
    long long rows_epoch = 0;

    // Structure summaries for this epoch (INC-3 presence, INC-4 kinds and ties).
    std::set<std::pair<int, std::string>>           present_now;
    /// Vet trigger for measurement admission (its INC-4,
    /// docs/SPEC_measurement_admission.md §10): the (station index, satellite)
    /// cells whose worst established @c obs_code deviation exceeded
    /// @ref pc_event_sigma this epoch.  Published below into
    /// `ctx.carry.prefit_code_flag`.  The admission seam runs earlier in the epoch
    /// than this walk, so it reads the flags one epoch late; the census flagged
    /// exposed re-entries 1-4 epochs before their |z| crossed the admission bound,
    /// so that latency is available.
    std::set<std::pair<int, std::string>>           code_flag_now;
    std::map<int, int>                              tie_now;
    std::map<std::pair<int, std::string>, unsigned> kind_now;
    const bool birth_on = birth_sigma > 0.0;
    const bool have_prior_presence = ctx.carry.prefit_walk_seeded;

    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;
        const std::string& sname = stations[i].name;
        ++ctr.station_epochs;

        std::vector<ClassifiedRow> tags;
        classifyStation(R, tags);
        const PrefitStationCentres c = centresFrom(R, tags);
        for (int t = 0; t < kNetDataClasses; ++t)
            if (!c.have[t] && c.n[t] > 0) ++ctr.type_skips;

        // A row's satellite is the one its tag names.  An SH<->slant
        // tie row touches only SH shared columns, and its tag names its own line of
        // sight: for a whitened tie row, the line of sight of its primary row (the
        // diagonal of L⁻¹).  Before the tags the satellite was recovered from the first
        // slant-ionosphere column the row touched, which for a whitened row is the
        // lowest such column, not the row's own.

        // Established cells (INC-1) and cells born this epoch (INC-3) are kept
        // apart.  A birth's innovation is set by the seed, which makes it about 0
        // against a station level that may be 1e5 m, so as an INC-1 exceedance it
        // would read as a 1e7-sigma false positive.  See "Births are not
        // exceedances" in the header.
        std::map<std::pair<std::string, int>, CellWorst> worst, worst_born;
        std::set<std::string> born_here;
        // Every walked station gets a tie-row entry, including zero.  Without it a
        // station whose ties are all cut drops out of the map, and its re-coupling
        // edge (0 -> n tie rows) reads as a first sighting and is never evented;
        // invariant #7 requires that edge to be logged.
        if (topo_on) tie_now[static_cast<int>(i)] = 0;

        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            const ClassifiedRow& tg = tags[static_cast<std::size_t>(row)];
            const int ti = dataClassIndex(tg.cls);
            if (ti < 0) continue;                   // pins / coupling excluded
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double y = R.y(row);
            if (!std::isfinite(y) || !std::isfinite(w)) { ++ctr.nonfinite_rows; continue; }
            ++ctr.rows;
            ++rows_epoch;

            // --- structure bookkeeping (INC-3 presence, INC-4 kinds/ties) ---
            const std::string& row_sat = tg.sat;
            const std::string sat = row_sat.empty() ? std::string("?") : row_sat;
            if (!row_sat.empty()) {
                const auto cell = std::make_pair(static_cast<int>(i), row_sat);
                present_now.insert(cell);
                if (topo_on) kind_now[cell] |= (1u << ti);
                // Births are detected whenever the walk runs, not only when INC-3
                // is armed.  INC-1 defers them either way; a deferral conditional
                // on INC-3 would make the census numbers depend on configuration.
                if (have_prior_presence
                    && !net::hasLos(ctx.entities.los, cell.first, cell.second,
                                    &net::LosState::prefit_present_prev)) {
                    born_here.insert(row_sat);
                    ++ctr.birth_rows;
                }
            }
            if (topo_on && ti == static_cast<int>(NetRowClass::IonoShRow))
                ++tie_now[static_cast<int>(i)];

            // --- the statistic ------------------------------------------------
            if (!c.have[ti]) continue;              // no trustworthy centre this epoch
            const double s = prefitStandardizedDeviation(y, w, c.m[ti]);
            if (born_here.count(sat)) {
                // Born this epoch: handed to INC-3 and kept out of INC-1's pools, so
                // max_s, p99 and the >1000 counts describe established cells.
                // Counted in rows_born_deferred.
                ++ctr.rows_born_deferred;
                CellWorst& bw = worst_born[{sat, ti}];
                if (s > bw.s) bw = CellWorst{s, y, w, c.m[ti], row};
                continue;
            }
            s_pool[ti].push_back(s);
            if (s > max_s[ti]) max_s[ti] = s;
            if (s > 10.0)   ++n_gt_10[ti];
            if (s > 1000.0) ++n_gt_1k[ti];
            if (s > ctr.max_s) {
                ctr.max_s = s;
                ctr.max_s_tow = se.time.tow;
                ctr.max_s_id = sname + "|" + sat + "|"
                             + rowClassName(static_cast<NetRowClass>(ti));
            }
            CellWorst& cw = worst[{sat, ti}];
            if (s > cw.s) cw = CellWorst{s, y, w, c.m[ti], row};
        }

        // --- exceedance events (INC-1; established cells, rate-limited) -------
        for (const auto& [key, cw] : worst) {
            if (ev_sigma <= 0.0 || cw.s <= ev_sigma) continue;
            // --- vet trigger for measurement admission ----------------
            // Recorded from the exceedance itself, ahead of `census_on`, the
            // event-class mask and the per-cell holdoff.  As with birth detection,
            // the detection does not depend on which consumers are armed, and a
            // trigger behind the diagnostic rate limiter would go quiet while a
            // cell stays bad, which is the case it exists for.
            //
            // Code rows only: a phase row's prefit innovation is arc-specific and
            // its station median is not a common mode (on one measured leg most
            // phase events were not faults), so a phase-driven trigger feeding an
            // admission verdict would produce false positives.  Born cells never
            // enter `worst` (they go to INC-3), and that exclusion matters: a
            // re-entering arc's ambiguity is fresh, so its return epoch reads as a
            // birth and is deferred.  The earliest census flag on a re-entry is
            // therefore the epoch after the re-entry.
            if (key.second == static_cast<int>(NetRowClass::ObsCode))
                code_flag_now.insert({static_cast<int>(i), key.first});
            if (!census_on) continue;
            const char* cls = rowClassName(static_cast<NetRowClass>(key.second));
            ++ctr.exceedances;
            // Event domain (network.prefit_event_classes).  The statistic and the
            // per-class columns cover every DATA class; only the event channel is
            // narrowed.  A phase row's prefit innovation is arc-specific (it
            // carries that arc's ambiguity linearization point), so its station
            // "median" is not a common mode; on one measured leg 608 of 665 events
            // were phase rows and none was a fault.  Suppressed events are counted.
            if ((ctx.knobs.pc_event_mask & (1u << static_cast<unsigned>(key.second))) == 0) {
                ++ctr.events_class_skipped;
                continue;
            }
            if (holdoffAllows(ctx, "exc", sname, key.first, cls, seq, holdoff)) {
                ++ctr.events;
                traces.emitEvent(se.time, "PREFIT", "prefit_exceedance", key.first,
                                 sname, cw.s, ev_sigma, "flag",
                                 fmt::format("class={} row={} y={:.6g} m={:.6g} "
                                             "w={:.6g}",
                                             cls, cw.row, cw.y, cw.m, cw.w));
            } else {
                ++ctr.events_held;
            }
        }
        // --- seed-birth tripwire (INC-3; born cells, tighter threshold) -------
        // These are the cells INC-1 handed over.  The hand-over count is kept
        // whether or not INC-3 is armed, so a quiet census and a census that
        // deferred births to a disarmed INC-3 can be told apart at run end.
        for (const auto& [key, cw] : worst_born) {
            const char* cls = rowClassName(static_cast<NetRowClass>(key.second));
            if (census_on && ev_sigma > 0.0 && cw.s > ev_sigma) ++ctr.cells_born_deferred;
            // Birth events use INC-1's event domain (network.prefit_event_classes),
            // for one domain to reason about and for the same measured reason: on one
            // birth leg 72 of 74 birth exceedances were obs_phase rows seeded to
            // y = 0, while the two real seed faults were obs_code rows born already
            // carrying about 2e5 m.
            if (birth_on && cw.s > birth_sigma
                && (ctx.knobs.pc_event_mask & (1u << static_cast<unsigned>(key.second))) == 0) {
                ++ctr.events_class_skipped;
                continue;
            }
            if (birth_on && cw.s > birth_sigma) {
                ++ctr.birth_events;
                // Seed values: this epoch's ambiguity linearization point(s) and the
                // slant-iono state.  A correct self-seeded birth has s ≈ 0, so an
                // implausible value here identifies the defective seed path.
                std::string seeds;
                for (const auto& pa : R.pl_amb) {
                    if (pa.sat != key.first) continue;
                    if (!seeds.empty()) seeds += ";";
                    seeds += fmt::format("amb[{}]={:.6g}", pa.signal, pa.x_lin);
                }
                try {
                    const StationParams& params = stations[i].params;
                    const auto sat_id = SatId::parse(key.first);
                    const int idx = sat_id ? findParam(params, slantIonoRef(*sat_id)) : -1;
                    if (idx >= 0) {
                        if (!seeds.empty()) seeds += ";";
                        seeds += fmt::format("slant={:.6g}",
                                             params.slots[static_cast<std::size_t>(idx)].x_lin);
                    }
                } catch (...) { /* diagnostics degrade, never abort the run */ }
                traces.emitEvent(se.time, "PREFIT", "seed_birth_exceedance", key.first,
                                 sname, cw.s, birth_sigma, "flag",
                                 fmt::format("class={} row={} y={:.6g} m={:.6g} "
                                             "w={:.6g} {}",
                                             cls, cw.row, cw.y, cw.m, cw.w,
                                             seeds.empty() ? "seeds=?" : seeds));
            }
        }
        // Births are a denominator for all increments, not only INC-3 (a quiet
        // census means nothing without the count it handed over), so they are
        // counted whenever the walk runs.
        ctr.births += static_cast<long long>(born_here.size());
    }

    if (!have_prior_presence)
        ctr.birth_seed_suppressed += static_cast<long long>(present_now.size());

    // --- epoch scalars for the epoch-status row (INC-1) ----------------------
    if (census_on) {
        for (int t = 0; t < kNetDataClasses; ++t) {
            census_ev.ep_pc_max_s[t]     = max_s[t];
            census_ev.ep_pc_n_gt_10[t]   = n_gt_10[t];
            census_ev.ep_pc_n_gt_1000[t] = n_gt_1k[t];
            std::vector<double>& p = s_pool[t];
            if (p.empty()) { census_ev.ep_pc_p99_s[t] = 0.0; continue; }
            const std::size_t k =
                static_cast<std::size_t>(std::floor(0.99 * static_cast<double>(p.size() - 1)));
            std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(k), p.end());
            census_ev.ep_pc_p99_s[t] = p[k];
        }
        census_ev.ep_pc_rows = rows_epoch;
    }

    // --- topology ledger (INC-4) --------------------------------------------
    if (topo_on) {
        auto emitTopo = [&](const char* event, const std::string& sat,
                            const std::string& station, std::optional<double> stat,
                            std::optional<double> thr, const std::string& detail) {
            traces.emitEvent(se.time, "TOPOLOGY", event, sat, station, stat, thr,
                             "delta", detail);
        };
        if (ctx.carry.topo_seeded) {
            // (a) SH-tie row-count delta per station.
            for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
                if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
                const int idx = static_cast<int>(i);
                const int* pit = net::get(ctx.entities.stations, StationId{idx},
                                          &net::StationState::topo_prev_tie);
                if (pit == nullptr) continue;   // first sighting
                const auto nit = tie_now.find(idx);
                const int now = (nit == tie_now.end()) ? 0 : nit->second;
                if (now == *pit) continue;
                ++ctr.topo_tie_events;
                emitTopo("tie_rows", "", stations[i].name,
                         static_cast<double>(now), static_cast<double>(*pit),
                         fmt::format("prev={} now={} delta={:+d}",
                                     *pit, now, now - *pit));
            }
            // (b) per-(station, satellite) row-kind changes while the sat stays present.
            for (const auto& [cell, mask] : kind_now) {
                const unsigned* pit = net::getLos(ctx.entities.los, cell.first, cell.second,
                                                  &net::LosState::topo_prev_kind);
                if (pit == nullptr) continue;   // rise, not a change
                if (*pit == mask) continue;
                ++ctr.topo_kind_events;
                const std::string sname =
                    (cell.first >= 0 && static_cast<std::size_t>(cell.first) < stations.size())
                        ? stations[static_cast<std::size_t>(cell.first)].name : std::string();
                emitTopo("row_kinds", cell.second, sname, std::nullopt, std::nullopt,
                         fmt::format("prev={} now={}", kindMaskName(*pit),
                                     kindMaskName(mask)));
            }
            // (c) shared-column registry deltas.
            std::set<std::string> cols_now(cm.label.begin(), cm.label.end());
            int added = 0, removed = 0;
            for (const std::string& L : cols_now) {
                if (ctx.carry.topo_prev_cols.count(L)) continue;
                ++ctr.topo_col_add;
                if (added++ >= kTopoColEventCap) { ++ctr.topo_truncated; continue; }
                emitTopo("col_added", "", "", std::nullopt, std::nullopt, "label=" + L);
            }
            for (const std::string& L : ctx.carry.topo_prev_cols) {
                if (cols_now.count(L)) continue;
                ++ctr.topo_col_rm;
                if (removed++ >= kTopoColEventCap) { ++ctr.topo_truncated; continue; }
                emitTopo("col_removed", "", "", std::nullopt, std::nullopt, "label=" + L);
            }
            if (added > kTopoColEventCap || removed > kTopoColEventCap)
                log->info("Network (topology): TOW {:.1f} shared-column churn "
                          "{} added / {} removed — only the first {} of each were "
                          "emitted as events (the run-end totals carry the rest).",
                          se.time.tow, added, removed, kTopoColEventCap);
            ctx.carry.topo_prev_cols = std::move(cols_now);
        } else {
            ctx.carry.topo_prev_cols.clear();
            ctx.carry.topo_prev_cols.insert(cm.label.begin(), cm.label.end());
            ctx.carry.topo_seeded = true;
            log->info("Network (topology): ledger SEEDED at TOW {:.1f} with {} station(s), "
                      "{} shared column(s) — deltas are reported from the next processed "
                      "epoch (a first sighting is not a topology change).",
                      se.time.tow, tie_now.size(), cm.label.size());
        }
        net::dropAll(entities.stations, &net::StationState::topo_prev_tie);
        for (const auto& [idx, n] : tie_now)
            net::put(entities.stations, StationId{idx}, &net::StationState::topo_prev_tie) = n;
        net::assignLos(entities, &net::LosState::topo_prev_kind, kind_now);
    }

    // --- presence map advance (INC-3) ---------------------------------------
    net::assignLosFlags(entities, &net::LosState::prefit_present_prev, present_now);
    ctx.carry.prefit_walk_seeded  = true;

    // --- publish the admission vet trigger ----------------------------------
    // Overwritten (not merged) on every walked epoch and stamped with the
    // loop-epoch index, so a consumer can tell "no cell flagged this epoch" from
    // "the walk did not run and the flags are older".  The admission seam accepts
    // only the stamp `n_loop_epochs - 1` and counts anything older as an absent
    // consultation.  Both seams read `n_loop_epochs`, incremented once per loop
    // epoch before either runs, so the one-epoch offset is exact.
    net::assignLosFlags(entities, &net::LosState::prefit_code_flag, code_flag_now);
    ctx.carry.prefit_code_flag_ep = seq;

    // --- invariant #4: an inert walk reports that it is inert -----------------
    if (rows_epoch == 0 && !ctx.counters.prefit_empty_logged) {
        ctx.counters.prefit_empty_logged = true;
        log->warn("Network (prefit census): ARMED but the first walked epoch (TOW "
                  "{:.1f}) classified ZERO data rows — every station was skipped (no "
                  "solve membership or a shared-dimension mismatch).  Every census "
                  "column will stay blank and no PREFIT event can ever fire.",
                  se.time.tow);
    }
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the prefit tier: census, fuse, seed births, topology, recorder, attribution.
int networkPrefitCensusRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkConfig& nc = v.config.getNetworkConfig();

    // --- Prefit census / fuse / seed-birth / topology / flight-recorder --------
    // (docs/SPEC_prefit_census_observability.md §10).  A tripwire that never fires and
    // a tripwire that was never connected produce identical logs (invariant #4).  Only
    // the denominators (`epochs`, `station_epochs`, `rows`, `rec_consults`) separate
    // them, so they are reported first, and every zero-finding line says which case
    // it is.
    const auto& p = ctx.counters.pc;
    if (ctx.knobs.pc_walk_on) {
        log->info("Network (prefit census): {} walk pass(es) over {} station-epoch(s), "
                  "{} DATA row(s) classified; {} (station, epoch, type) median(s) "
                  "skipped for population < {}, {} non-finite row(s) dropped.",
                  p.epochs, p.station_epochs, p.rows, p.type_skips,
                  kNetPrefitMinTypeRows, p.nonfinite_rows);
        if (ctx.knobs.pc_census_on) {
            log->info("Network (prefit census INC-1): {} row-cell exceedance(s) over "
                      "s = {:g} on ESTABLISHED cells; {} event(s) emitted, {} "
                      "suppressed by the {}-epoch per-cell holdoff, {} outside the "
                      "event domain [{}].  Run maximum s = {:.4g}{}.",
                      p.exceedances, ctx.knobs.pc_event_sigma, p.events,
                      p.events_held, ctx.knobs.pc_event_holdoff,
                      p.events_class_skipped,
                      dataClassMaskName(ctx.knobs.pc_event_mask), p.max_s,
                      p.max_s_id.empty()
                          ? std::string()
                          : fmt::format(" on {} at TOW {:.1f}", p.max_s_id,
                                        p.max_s_tow));
            // The hand-off to INC-3 is reported whether or not INC-3 is armed:
            // "the census went quiet" and "the census deferred N births to a
            // disarmed birth check" are different states and must not read the same
            // (invariant #4, applied to a filter rather than to wiring).
            log->info("Network (prefit census INC-1): {} row(s) in {} born cell(s) "
                      "were DEFERRED to INC-3 rather than counted here — a fresh "
                      "arc is seeded to ~0 innovation against a station level that "
                      "can be 1e5 m, so an established-cell statistic is the only "
                      "one that means anything.  {}",
                      p.rows_born_deferred, p.cells_born_deferred,
                      (ctx.knobs.pc_birth_sigma > 0.0)
                          ? "INC-3 is armed and graded them."
                          : "INC-3 is OFF, so nothing graded them — arm "
                            "network.seed_birth_check_sigma to see what was in "
                            "there.");
            if (p.exceedances == 0 && p.rows > 0)
                log->info("Network (prefit census INC-1): {} row(s) walked and NOT ONE "
                          "exceeded s = {:g}.  That is a genuine negative — no cell in "
                          "this run carried a fault-scale common-mode-removed "
                          "innovation — and it is also the state in which the tripwire "
                          "cannot demonstrate that it works.  Grade it on a leg with a "
                          "known injector.", p.rows, ctx.knobs.pc_event_sigma);
        }
        if (ctx.knobs.pc_birth_sigma > 0.0) {
            log->info("Network (prefit census INC-3): {} LOS birth(s) over {} row(s), "
                      "{} seed_birth_exceedance event(s) at s > {:g}; {} first-epoch "
                      "presence(s) suppressed (no prior map — a first sighting is not "
                      "a birth).",
                      p.births, p.birth_rows, p.birth_events,
                      ctx.knobs.pc_birth_sigma, p.birth_seed_suppressed);
            if (p.births == 0)
                log->warn("Network (prefit census INC-3): ARMED but ZERO births were "
                          "detected all run — with satellites rising and setting that "
                          "is a MISWIRE (the presence map never advanced), not a quiet "
                          "network.");
            else if (p.birth_events == 0)
                log->info("Network (prefit census INC-3): {} birth(s) and NOT ONE fired "
                          "— every self-seeded LOS started consistent with its "
                          "network.  That RULES OUT the seed path for this window; a "
                          "mid-arc poisoning mechanism owns whatever came later.",
                          p.births);
        }
        if (ctx.knobs.pc_topo_on) {
            log->info("Network (prefit census INC-4): topology events — {} SH-tie "
                      "row-count delta(s), {} (station, sat) row-kind change(s), "
                      "{} shared column(s) added / {} removed{}.",
                      p.topo_tie_events, p.topo_kind_events, p.topo_col_add,
                      p.topo_col_rm,
                      p.topo_truncated > 0
                          ? fmt::format(" ({} column event(s) TRUNCATED by the "
                                        "per-epoch cap of {} — the totals here are "
                                        "complete, the event rows are not)",
                                        p.topo_truncated, 64)
                          : std::string());
            if (p.topo_tie_events == 0 && p.epochs > 1)
                log->info("Network (prefit census INC-4): NOT ONE station's SH-tie row "
                          "set changed all run.  This run therefore contains no instance "
                          "of the era-2 topology event and cannot be used to grade the "
                          "ledger against it.");
        }
        if (p.epochs == 0)
            log->warn("Network (prefit census): ARMED but the walk ran on ZERO epochs "
                      "— the wiring never executed.  Nothing in this run is evidence "
                      "about the prefit census.");
        else if (p.rows == 0)
            log->warn("Network (prefit census): ARMED, {} walk(s) ran and ZERO DATA "
                      "rows were classified — every station was skipped (no solve "
                      "membership or a shared-dimension mismatch).  The census is "
                      "INERT despite looking live.", p.epochs);
    }
    if (ctx.knobs.pc_fuse_sigma > 0.0) {
        log->info("Network (prefit fuse INC-2): {} epoch(s) x {} station-epoch(s), "
                  "{} DATA row(s) screened at S_fuse = {:g}; {} row(s) CAPPED "
                  "(deepest cut w_eff/w = {:.3g}, largest pre-cap s = {:.4g}), "
                  "{} event(s) emitted / {} held.  Domain [{}]: {} row(s) skipped "
                  "out of domain, of which {} WOULD have been capped.  Skipped: {} "
                  "pin-weight row(s) (w >= {:g}), {} row(s) whose station "
                  "type-median was unavailable.",
                  p.fuse_epochs, p.fuse_station_epochs, p.fuse_rows,
                  ctx.knobs.pc_fuse_sigma, p.fuse_capped, p.fuse_min_ratio,
                  p.fuse_max_s, p.fuse_events, p.fuse_events_held,
                  dataClassMaskName(ctx.knobs.pc_fuse_mask), p.fuse_skip_class,
                  p.fuse_would_class,
                  p.fuse_skip_pin_w, kNetPrefitPinWeight, p.fuse_skip_thin);
        if (p.fuse_rows == 0)
            log->warn("Network (prefit fuse INC-2): ARMED but ZERO rows were screened "
                      "— the assembly hook never ran.  The solve in this run is the "
                      "UNFUSED one; do not report it as a fuse arm.");
        else if (p.fuse_capped == 0)
            log->info("Network (prefit fuse INC-2): {} row(s) screened and NOT ONE "
                      "exceeded S_fuse = {:g} — this arm is byte-identical to its "
                      "control by construction, which is the correct null result but "
                      "is NOT evidence the cap works.",
                      p.fuse_rows, ctx.knobs.pc_fuse_sigma);
    }
    if (ctx.knobs.rec_on) {
        log->info("Network (rhs-attrib INC-5): flight recorder — {} trigger-site "
                  "consultation(s), {} era trip(s), {} dump(s) written ({} of them "
                  "retained PREVIOUS-epoch frames); {} epoch(s) retained, last frame "
                  "{:.1f} MB.",
                  p.rec_consults, p.rec_trips, p.rec_dumps, p.rec_prev_dumps,
                  p.rec_frames_retained,
                  static_cast<double>(p.rec_prev_bytes_last) / (1024.0 * 1024.0));
        if (p.rec_consults == 0)
            log->warn("Network (rhs-attrib INC-5): ARMED but NO trigger site ever "
                      "consulted the latch — the three trip sites (sigma0 withhold, "
                      "clock runaway, a00 band) are all gated behind "
                      "their own feature toggles, and none of them ran.  The recorder "
                      "is DEAD WIRING in this configuration, not a quiet run.");
        else if (p.rec_trips == 0)
            log->info("Network (rhs-attrib INC-5): {} consultation(s) and NOT ONE era "
                      "tripped — the run stayed inside every QC band.  The recorder "
                      "had ZERO opportunity, which is not evidence that it works.",
                      p.rec_consults);
        if (ctx.knobs.rec_on && nc.rhs_attrib_on_trip_prev
            && p.rec_frames_retained == 0)
            log->warn("Network (rhs-attrib INC-5): previous-epoch retention is ON but "
                      "ZERO frames were captured — every epoch failed the capture "
                      "(no reduced system) or the network exceeded "
                      "network.rhs_attrib_prev_max_stations = {}.  A trip would dump "
                      "the aftermath only.", nc.rhs_attrib_prev_max_stations);
    }
    return 0;
}

// ---- End of the run-end reports ----

// ---- Epoch forms (generated by scripts/nom_ep1_cut.py): one forwarding
//      call each, with the bindings of the frozen call site; the explicit forms
//      above stay for their other callers. ----

void networkPrefitCensus(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events) {
    networkPrefitCensus(run.ctx, run.config, run.log, run.traces, ep.se, run.setup.stations,
                        run.engines.est, ep.assembly.asm_res, ep.assembly.est_index_of, events);
}

// ---- End of the epoch forms ----

// ---- Solve stages (generated by scripts/nom_sg_cut.py) ----
/// The census-fed H sigma0 of this epoch, before any H line (Doxygen at the declaration).
void networkCensusSigma0(NetworkRun& run, const NetworkEpoch& ep,
                         const std::vector<int>& est_index_of) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkEstimator& est = run.engines.est;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    auto& stations = setup.stations;
    // ---- End of the bindings ----
    // The census-fed H σ̂₀ is computed once per epoch, before the solve, so every H
    // site downstream (including the pre-solve observability-gate withhold further
    // down) reads the same value.  Its inputs, the asm_res row stacks, are fixed at
    // assembly, and the value is measure-only.  0.0 is the H grammar's "unavailable"
    // (empty pool); the reset keeps a knob-off run at an inert 0 rather than a stale
    // value (H-rider, edge-queue §1.6 item 3a).
    ctx.carry.ep_h_sigma0_census = 0.0;
    if (ctx.knobs.h_sigma0_census) {
        ctx.carry.ep_h_sigma0_census = networkPrefitSigma0(
            ctx, stations, est, asm_res, est_index_of, ctx.knobs.h_sigma0_class_mask);
        if (ctx.carry.ep_h_sigma0_census > 0.0) ++ctx.counters.h_sig0_census_nonzero;
    }
}
// ---- End of the solve stages ----

// ---- PASS B serial stages (generated by scripts/nom_pb1_cut.py) ----

void networkPrefitFuseEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochAssembly& assembly) {
    // ---- From the run and the epoch (generated by scripts/nom_pb1_cut.py,
    //      each line checked by scripts/nom_pb1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const std::vector<bool>& present = ep.pass_a.present;
    std::vector<StationAssembly>& asm_res = assembly.asm_res;  // written
    // ---- End of the bindings ----
    // The alias lines of networkPassB_ this stage reads, verbatim.
    auto& stations = setup.stations;

    // --- Structural fuse (docs/SPEC_prefit_census_observability.md §4, INC-2) ---
    // The single last-touch site for every data row's weight: every appender
    // (splitStation, the GLONASS anchors, the SH/GIM iono ties, the SD-EWL rows)
    // has finished, and nothing between here and est.addStation writes R.w.  Serial
    // by design: the row build is parallel, so a fuse inside it would make the event
    // order depend on the thread count.  s_fuse <= 0 (the default) skips the whole
    // block (no classification, no allocation), so the off path is structurally
    // byte-identical.  The rows are classified from their tags.
    const double pc_fuse_sigma = ctx.knobs.pc_fuse_sigma;
    if (pc_fuse_sigma > 0.0) {
        const Eigen::Index fuse_n_shared = est.numShared();
        if (fuse_n_shared > 0) {
            ++ctx.counters.pc.fuse_epochs;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StationAssembly& R = asm_res[i];
                if (!R.ok || R.threw || R.state_mismatch || R.empty_innov) continue;
                networkPrefitFuseStation(ctx, traces, se.time, fuse_n_shared,
                                         stations[i].name, R, pc_fuse_sigma);
            }
        }
    }
}

// ---- End of the PASS B serial stages ----

} // namespace GnssProcessor
