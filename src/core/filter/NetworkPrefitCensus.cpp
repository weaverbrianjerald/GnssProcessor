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
/// start registers hundreds of columns at once and the ledger is meant to make
/// STEADY-STATE flips visible, not to transcribe the first epoch.  Truncation
/// is counted and reported at run end — never silent (invariant #4 / the
/// "no silent caps" rule).
constexpr int kTopoColEventCap = 64;

/// One classified row: its class plus the satellite of the first labelled
/// shared column it touches.
struct RowTag {
    NetRowClass cls{NetRowClass::ObsOther};
    std::string sat;
};

/// Classify every row of one station once (the walk's only O(rows × cols) pass).
void classifyStation(const SharedColumnMap& cm, const StationAssembly& R,
                     std::vector<RowTag>& tags) {
    const Eigen::Index m = R.y.size();
    tags.assign(static_cast<std::size_t>(std::max<Eigen::Index>(m, 0)), RowTag{});
    for (Eigen::Index row = 0; row < m; ++row) {
        RowTag& tg = tags[static_cast<std::size_t>(row)];
        tg.cls = classifyRow(cm, R, row, &tg.sat);
    }
}

/// Robust per-type centres from a pre-classified station (the shared core of
/// @ref prefitStationCentres and of both walks).
PrefitStationCentres centresFrom(const StationAssembly& R,
                                 const std::vector<RowTag>& tags) {
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
        // iono_sh_row carries no receiver clock ⇒ NOT centred (m ≡ 0).
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

/// Rate-limited event gate: true when this (event, station, sat, class) cell may
/// speak again.  A parked cell must log its identity PERSISTENTLY (so a later
/// reader can see how long it sat there) but not 120x/hour.
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

PrefitStationCentres prefitStationCentres(const SharedColumnMap& cm,
                                          const StationAssembly& R) {
    std::vector<RowTag> tags;
    classifyStation(cm, R, tags);
    return centresFrom(R, tags);
}

// =====================================================================
//  INC-2 — the structural fuse
// =====================================================================

int networkPrefitFuseStation(NetworkRunContext& ctx, NetworkRunTraces& traces,
                             const GnssTime& t, const SharedColumnMap& cm,
                             const std::string& station, StationAssembly& R,
                             double s_fuse) {
    if (s_fuse <= 0.0 || !R.ok || R.y.size() == 0 || R.A_s.cols() != cm.n) return 0;
    auto& ctr = ctx.counters.pc;
    ++ctr.fuse_station_epochs;

    std::vector<RowTag> tags;
    classifyStation(cm, R, tags);
    const PrefitStationCentres c = centresFrom(R, tags);

    const long long seq = static_cast<long long>(ctx.counters.n_loop_epochs);
    const long long holdoff = ctx.knobs.pc_event_holdoff;
    std::map<std::pair<std::string, int>, CellWorst> worst;   // pre-cap identity
    int capped = 0;

    for (Eigen::Index row = 0; row < R.y.size(); ++row) {
        const RowTag& tg = tags[static_cast<std::size_t>(row)];
        const int ti = dataClassIndex(tg.cls);
        if (ti < 0) continue;                       // pins / coupling: never fused
        ++ctr.fuse_rows;
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const double y = R.y(row);
        if (!std::isfinite(y) || !std::isfinite(w) || w <= 0.0) continue;
        if (w >= kNetPrefitPinWeight) { ++ctr.fuse_skip_pin_w; continue; }
        if (!c.have[ti])              { ++ctr.fuse_skip_thin;  continue; }

        const double s = prefitStandardizedDeviation(y, w, c.m[ti]);
        // Domain restriction (network.prefit_fuse_classes) — counted BOTH ways:
        // `fuse_skip_class` is the exposure and `fuse_would_class` is what the cap
        // gave up, so narrowing the domain is a measured trade rather than an
        // invisible one.  See the knob's doc for why phase is out by default.
        if ((ctx.knobs.pc_fuse_mask & (1u << static_cast<unsigned>(ti))) == 0) {
            ++ctr.fuse_skip_class;
            if (s > s_fuse) ++ctr.fuse_would_class;
            continue;
        }
        if (s <= s_fuse) continue;

        // Continuous cap: the row contributes at most an S_fuse-equivalent
        // standardized load.  NOT a drop — order-preserving, smooth, and the
        // row still carries its sign (the prefer-fallback directive).
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
//  INC-1 / INC-3 / INC-4 — the post-solve walk
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
    // Clear FIRST: an epoch the walk cannot serve must leave the epoch-status
    // columns blank, never repeat the previous epoch's numbers as if they were
    // this epoch's measurement.
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

    // INC-3 / INC-4 structure summaries for THIS epoch.
    std::set<std::pair<int, std::string>>           present_now;
    /// The admission card's INC-4 vet trigger (docs/SPEC_measurement_admission.md
    /// §10): (station index, satellite) whose worst ESTABLISHED @c obs_code
    /// deviation exceeded @ref pc_event_sigma this epoch.  Published into
    /// `ctx.carry.prefit_code_flag` below for the admission seam, which runs
    /// EARLIER in the epoch than this walk and therefore reads it one epoch late —
    /// which is exactly the latency the field data shows is available: the census
    /// named all four of the FT63 day's exposed re-entries 1-4 epochs before their
    /// |z| crossed the admission bound.
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

        std::vector<RowTag> tags;
        classifyStation(cm, R, tags);
        const PrefitStationCentres c = centresFrom(R, tags);
        for (int t = 0; t < kNetDataClasses; ++t)
            if (!c.have[t] && c.n[t] > 0) ++ctr.type_skips;

        // An SH<->slant TIE row touches only SH shared columns, so the shared
        // labels cannot name its satellite — and an unnamed tie-row event is
        // half an event, exactly on the valve this card was written for.  The
        // satellite is recoverable from the LOCAL side: the row carries the
        // station's slant-iono column for that LOS.  Built once per station-epoch
        // (state key -> H column -> A_r column), and only while the walk runs.
        std::map<Eigen::Index, std::string> slant_col_sat;
        if (stations[i].sm) {
            try {
                for (const auto& [sat, sidx] : stations[i].sm->getIonosphereStates()) {
                    const auto hit = R.h2local.find(static_cast<int>(sidx));
                    if (hit != R.h2local.end())
                        slant_col_sat[static_cast<Eigen::Index>(hit->second)] = sat;
                }
            } catch (...) { slant_col_sat.clear(); }   // diagnostics degrade, never abort
        }
        auto slantSat = [&](Eigen::Index row) -> std::string {
            for (const auto& [col, sat] : slant_col_sat)
                if (col >= 0 && col < R.A_r.cols() && R.A_r(row, col) != 0.0) return sat;
            return {};
        };

        // ESTABLISHED cells (INC-1) and cells BORN this epoch (INC-3) are kept
        // apart: a birth's innovation is arbitrary by construction — the seed
        // makes it ~0 against a station level that may be 1e5 m — so counting it
        // as an INC-1 exceedance is a false positive at 1e7 sigma, 666 times an
        // hour.  See the header's "births are not exceedances".
        std::map<std::pair<std::string, int>, CellWorst> worst, worst_born;
        std::set<std::string> born_here;
        // Every walked station gets a tie-row entry, INCLUDING zero.  Without
        // this a station whose ties are all cut drops out of the map, so the
        // RE-COUPLING edge (0 -> n, the era-2 discharge trigger) would be read as
        // a first sighting and never evented — the exact half of the valve that
        // proposed invariant #7 says must not be silent.
        if (topo_on) tie_now[static_cast<int>(i)] = 0;

        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            const RowTag& tg = tags[static_cast<std::size_t>(row)];
            const int ti = dataClassIndex(tg.cls);
            if (ti < 0) continue;                   // pins / coupling excluded
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double y = R.y(row);
            if (!std::isfinite(y) || !std::isfinite(w)) { ++ctr.nonfinite_rows; continue; }
            ++ctr.rows;
            ++rows_epoch;

            // --- structure bookkeeping (INC-3 presence, INC-4 kinds/ties) ---
            std::string row_sat = tg.sat;
            if (row_sat.empty() && tg.cls == NetRowClass::IonoShRow)
                row_sat = slantSat(row);
            const std::string sat = row_sat.empty() ? std::string("?") : row_sat;
            if (!row_sat.empty()) {
                const auto cell = std::make_pair(static_cast<int>(i), row_sat);
                present_now.insert(cell);
                if (topo_on) kind_now[cell] |= (1u << ti);
                // Births are detected whenever the walk runs — NOT only when
                // INC-3 is armed.  INC-1 defers to them either way, and a
                // deferral that depended on whether INC-3 was listening would
                // make the census's own numbers config-dependent.
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
                // Born this epoch: hand it to INC-3 and keep it out of INC-1's
                // pools entirely, so max_s / p99 / the >1000 counts describe
                // ESTABLISHED cells.  Counted, not silently dropped.
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

        // --- INC-1 exceedance events (ESTABLISHED cells, rate-limited) -------
        for (const auto& [key, cw] : worst) {
            if (ev_sigma <= 0.0 || cw.s <= ev_sigma) continue;
            // --- the admission card's INC-4 vet trigger ------------------------
            // Recorded from the EXCEEDANCE itself, deliberately UPSTREAM of
            // `census_on`, of the event-class mask and of the per-cell holdoff.
            // This card's own principle is that a detection must not depend on
            // whether anyone is listening on the other side (§ "births are detected
            // whenever the walk runs — not only when INC-3 is armed"), and a trigger
            // throttled by a diagnostic rate limiter would go quiet exactly when a
            // cell is PERSISTENTLY bad, which is the case it exists for.
            //
            // CODE rows only: a phase row's prefit innovation is arc-specific and
            // its station median is not a common mode (608 of 665 phase events on
            // one leg, none of them findings), so a phase-driven trigger wired to an
            // admission verdict would be a false-positive generator.  BORN cells are
            // excluded for free — they never enter `worst`, they are handed to INC-3
            // — and that exclusion is load-bearing rather than incidental: a
            // re-entering arc's ambiguity IS fresh, so its own return epoch reads as
            // a birth and defers.  It is the reason the census's earliest possible
            // flag on a re-entry is re-entry + 1, which is exactly the lag measured
            // at G16@BSHM (resume 307740, flagged 307770).
            if (key.second == static_cast<int>(NetRowClass::ObsCode))
                code_flag_now.insert({static_cast<int>(i), key.first});
            if (!census_on) continue;
            const char* cls = rowClassName(static_cast<NetRowClass>(key.second));
            ++ctr.exceedances;
            // EVENT DOMAIN (network.prefit_event_classes).  The statistic and the
            // per-class columns cover every DATA class; only the event channel is
            // narrowed, because a phase row's prefit innovation is arc-specific
            // (it carries that arc's ambiguity linearization point) and its
            // station "median" is not a common mode at all — measured 608 of 665
            // events on one leg, none of them findings.  Suppression is COUNTED.
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
        // --- INC-3 seed-birth tripwire (BORN cells, tighter threshold) -------
        // These are the cells INC-1 handed over.  The count of what was handed
        // over is reported whether or not INC-3 is armed, so "the census went
        // quiet" and "the census deferred 400 births to a disarmed INC-3" are
        // distinguishable at run end.
        for (const auto& [key, cw] : worst_born) {
            const char* cls = rowClassName(static_cast<NetRowClass>(key.second));
            if (census_on && ev_sigma > 0.0 && cw.s > ev_sigma) ++ctr.cells_born_deferred;
            // INC-3 shares INC-1's EVENT DOMAIN (network.prefit_event_classes) —
            // one domain to reason about, and for the same measured reason: on the
            // snap_19 birth leg 72 of 74 birth exceedances were obs_phase rows
            // seeded to y = 0, while the TWO that mattered were obs_code and both
            // were (BSHM00ISR, G07/G16) born already carrying ~2e5 m.  A channel
            // where the signal is 3% of the traffic is not a locator.
            if (birth_on && cw.s > birth_sigma
                && (ctx.knobs.pc_event_mask & (1u << static_cast<unsigned>(key.second))) == 0) {
                ++ctr.events_class_skipped;
                continue;
            }
            if (birth_on && cw.s > birth_sigma) {
                ++ctr.birth_events;
                // Seed values: the ambiguity linearization point(s) this epoch and
                // the slant-iono state — a correct self-seeded birth has s ≈ 0, so
                // whichever of these is absurd IS the defective seed path.
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
        // Births are a DENOMINATOR for the whole card, not just for INC-3 —
        // "the census is quiet" means nothing without knowing how much it
        // handed over — so they are counted whenever the walk runs.
        ctr.births += static_cast<long long>(born_here.size());
    }

    if (!have_prior_presence)
        ctr.birth_seed_suppressed += static_cast<long long>(present_now.size());

    // --- INC-1 epoch scalars for the epoch-status row ------------------------
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

    // --- INC-4 topology ledger ----------------------------------------------
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
            // (a) SH-tie row-set presence delta — the DEWEIGHT VALVE's own edge.
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
            // (b) per-(station, satellite) row-KIND changes while the sat stays present.
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

    // --- publish the INC-4 vet trigger --------------------------------------
    // Overwritten (not merged) every walked epoch and stamped with the loop-epoch
    // index, so a consumer can tell "no cell was flagged this epoch" from "the walk
    // did not run and these flags are from some earlier epoch".  The admission
    // seam honours the stamp `n_loop_epochs - 1` only; anything older it counts as
    // an absent consultation rather than believing it.  Both seams read the SAME
    // counter (`n_loop_epochs`, incremented once per loop epoch before either runs),
    // so the one-epoch offset is exact rather than assumed.
    ctx.carry.prefit_code_flag    = std::move(code_flag_now);
    ctx.carry.prefit_code_flag_ep = seq;

    // --- invariant #4: an inert walk must SAY it is inert --------------------
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
