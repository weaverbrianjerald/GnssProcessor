/**
 * @file NetworkPrefitCensus.cpp
 * @brief Implementation of the prefit census / structural fuse / seed-birth
 *        tripwire / topology ledger (see NetworkPrefitCensus.hpp).
 */

#include "filter/NetworkPrefitCensus.hpp"

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

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"

namespace GnssProcessor {

namespace {

/// Per-epoch cap on shared-column add/remove events, each direction.  A cold
/// start registers hundreds of columns at once; the ledger is for steady-state
/// changes, not a transcript of the first epochs.  Truncated events are counted
/// (@c topo_truncated, reported at run end) and the epoch is logged
/// (invariant #4).
constexpr int kTopoColEventCap = 64;

/// One classified row: its class and the satellite its contribution tag names (card NOM
/// step N2b).  (Named apart from the declared contribution tag, RowTag of
/// filter/NetworkContributions.hpp, card NOM N2a.)
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
bool holdoffAllows(NetworkRunContext& ctx, const std::string& key, long long seq,
                   long long holdoff) {
    auto& last = ctx.carry.prefit_event_last;
    const auto it = last.find(key);
    if (it != last.end() && holdoff > 0 && (seq - it->second) < holdoff) return false;
    last[key] = seq;
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
            // its line of sight's satellite (card NOM step N2b keeps that rule).
            if (tg.cls != NetRowClass::IonoShRow && !tg.sat.empty() && have_prior
                && ctx.carry.prefit_present_prev.count(
                       {static_cast<int>(i), tg.sat}) == 0)
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
        const std::string hk = std::string("fused|") + station + "|" + key.first + "|" + cls;
        if (!holdoffAllows(ctx, hk, seq, holdoff)) { ++ctr.fuse_events_held; continue; }
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
                         const std::vector<int>& est_index_of,
                         const NetworkIonoDeweightGate* iono_deweight) {
    if (!ctx.knobs.pc_walk_on) return;
    (void)config_;   // arming is expressed entirely through the knob snapshot
    // Cleared first, so an epoch the walk cannot serve leaves the epoch-status
    // columns blank instead of repeating the previous epoch's numbers.
    ctx.carry.ep_pc_rows = 0;

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
    /// Vet trigger for the measurement-admission card (its INC-4,
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

        // A row's satellite is the one its tag names (card NOM step N2b).  An SH<->slant
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
                    && ctx.carry.prefit_present_prev.count(cell) == 0) {
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
            // --- vet trigger for the measurement-admission card ----------------
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
            const std::string hk =
                std::string("exc|") + sname + "|" + key.first + "|" + cls;
            if (holdoffAllows(ctx, hk, seq, holdoff)) {
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
                if (stations[i].sm) {
                    try {
                        const auto& iono = stations[i].sm->getIonosphereStates();
                        const auto it = iono.find(key.first);
                        if (it != iono.end()) {
                            const Eigen::VectorXd x = stations[i].sm->getStateVector();
                            const auto idx = static_cast<Eigen::Index>(it->second);
                            if (idx >= 0 && idx < x.size()) {
                                if (!seeds.empty()) seeds += ";";
                                seeds += fmt::format("slant={:.6g}", x(idx));
                            }
                        }
                    } catch (...) { /* diagnostics degrade, never abort the run */ }
                }
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
            ctx.carry.ep_pc_max_s[t]     = max_s[t];
            ctx.carry.ep_pc_n_gt_10[t]   = n_gt_10[t];
            ctx.carry.ep_pc_n_gt_1000[t] = n_gt_1k[t];
            std::vector<double>& p = s_pool[t];
            if (p.empty()) { ctx.carry.ep_pc_p99_s[t] = 0.0; continue; }
            const std::size_t k =
                static_cast<std::size_t>(std::floor(0.99 * static_cast<double>(p.size() - 1)));
            std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(k), p.end());
            ctx.carry.ep_pc_p99_s[t] = p[k];
        }
        ctx.carry.ep_pc_rows = rows_epoch;
    }

    // --- topology ledger (INC-4) --------------------------------------------
    if (topo_on) {
        auto emitTopo = [&](const char* event, const std::string& sat,
                            const std::string& station, std::optional<double> stat,
                            std::optional<double> thr, const std::string& detail) {
            traces.emitEvent(se.time, "TOPOLOGY", event, sat, station, stat, thr,
                             "delta", detail);
        };
        std::map<int, double> w_now;
        if (iono_deweight)
            for (std::size_t i = 0; i < stations.size(); ++i)
                w_now[static_cast<int>(i)] = iono_deweight->weight(stations[i].name);

        if (ctx.carry.topo_seeded) {
            // (a) SH-tie row-count delta per station: the edges of the iono deweight valve.
            for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
                if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
                const int idx = static_cast<int>(i);
                const auto pit = ctx.carry.topo_prev_tie.find(idx);
                if (pit == ctx.carry.topo_prev_tie.end()) continue;   // first sighting
                const auto nit = tie_now.find(idx);
                const int now = (nit == tie_now.end()) ? 0 : nit->second;
                if (now == pit->second) continue;
                const auto wp = ctx.carry.topo_prev_w.find(idx);
                const auto wn = w_now.find(idx);
                ++ctr.topo_tie_events;
                emitTopo("tie_rows", "", stations[i].name,
                         static_cast<double>(now), static_cast<double>(pit->second),
                         fmt::format("prev={} now={} delta={:+d} w_prev={:.4g} w_now={:.4g}",
                                     pit->second, now, now - pit->second,
                                     wp == ctx.carry.topo_prev_w.end() ? 1.0 : wp->second,
                                     wn == w_now.end() ? 1.0 : wn->second));
            }
            // (b) per-(station, satellite) row-kind changes while the sat stays present.
            for (const auto& [cell, mask] : kind_now) {
                const auto pit = ctx.carry.topo_prev_kind.find(cell);
                if (pit == ctx.carry.topo_prev_kind.end()) continue;   // rise, not a change
                if (pit->second == mask) continue;
                ++ctr.topo_kind_events;
                const std::string sname =
                    (cell.first >= 0 && static_cast<std::size_t>(cell.first) < stations.size())
                        ? stations[static_cast<std::size_t>(cell.first)].name : std::string();
                emitTopo("row_kinds", cell.second, sname, std::nullopt, std::nullopt,
                         fmt::format("prev={} now={}", kindMaskName(pit->second),
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
        ctx.carry.topo_prev_tie  = std::move(tie_now);
        ctx.carry.topo_prev_kind = std::move(kind_now);
        ctx.carry.topo_prev_w    = std::move(w_now);
    }

    // --- presence map advance (INC-3) ---------------------------------------
    ctx.carry.prefit_present_prev = std::move(present_now);
    ctx.carry.prefit_walk_seeded  = true;

    // --- publish the admission vet trigger ----------------------------------
    // Overwritten (not merged) on every walked epoch and stamped with the
    // loop-epoch index, so a consumer can tell "no cell flagged this epoch" from
    // "the walk did not run and the flags are older".  The admission seam accepts
    // only the stamp `n_loop_epochs - 1` and counts anything older as an absent
    // consultation.  Both seams read `n_loop_epochs`, incremented once per loop
    // epoch before either runs, so the one-epoch offset is exact.
    ctx.carry.prefit_code_flag    = std::move(code_flag_now);
    ctx.carry.prefit_code_flag_ep = seq;

    // --- invariant #4: an inert walk reports that it is inert -----------------
    if (rows_epoch == 0 && !ctx.carry.prefit_empty_logged) {
        ctx.carry.prefit_empty_logged = true;
        log->warn("Network (prefit census): ARMED but the first walked epoch (TOW "
                  "{:.1f}) classified ZERO data rows — every station was skipped (no "
                  "solve membership or a shared-dimension mismatch).  Every census "
                  "column will stay blank and no PREFIT event can ever fire.",
                  se.time.tow);
    }
}

} // namespace GnssProcessor
