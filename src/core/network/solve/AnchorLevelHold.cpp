/**
 * @file AnchorLevelHold.cpp
 * @brief An anchor station's level directions held as part of the phase datum (card ALD):
 *        the direction extraction, the hold row, the station step, the serial tally with the
 *        hold's edges as events, and the run-end report.  See
 *        network/solve/AnchorLevelHold.hpp.
 */

#include "network/solve/AnchorLevelHold.hpp"

#include "network/assembly/Contributions.hpp"   // RowTag
#include "network/assembly/Responses.hpp"       // carryPastHorizon
#include "network/assembly/StationBuild.hpp"    // StationBuild, networkGrowBlockRows
#include "network/diag/RunTraces.hpp"           // emitEvent
#include "network/driver/Epoch.hpp"             // EpochEvents, EpochClock
#include "network/driver/SetupResult.hpp"       // NetworkSetupResult::stations
#include "network/runend/RunEnd.hpp"            // NetworkRunEndView
#include "network/solve/PhaseDatum.hpp"         // networkIsGaugePinned
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "network/state/StationParams.hpp"      // slotAt, keyText, pendingReseed
#include "rinex/MultiRinexSync.hpp"             // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

// ---- The direction extraction ----

LevelDirections anchorLevelDirections(const Eigen::MatrixXd& A_data, const Eigen::VectorXd& w_data,
                                      const std::vector<System>& col_sys,
                                      const std::vector<char>& col_iono,
                                      const std::vector<System>& want) {
    LevelDirections out;
    const Eigen::Index nr = A_data.cols();
    const Eigen::Index m  = A_data.rows();
    const auto refuseAll = [&](LevelHoldSkip why) {
        for (System s : want) out.refused.emplace_back(s, why);
    };
    if (nr <= 0 || m <= 0) { refuseAll(nr <= 0 ? LevelHoldSkip::NoColumns : LevelHoldSkip::NoDataInformation); return out; }

    // The weighted normals of the data rows: the null space is that of the rows themselves
    // (every weight is positive), formed as the RHS attribution's level readout forms it.
    Eigen::MatrixXd Nd = Eigen::MatrixXd::Zero(nr, nr);
    for (Eigen::Index r = 0; r < m; ++r) {
        const double w = (r < w_data.size()) ? w_data(r) : 1.0;
        if (!std::isfinite(w) || w == 0.0) continue;
        Nd.noalias() += w * A_data.row(r).transpose() * A_data.row(r);
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nd);
    if (es.info() != Eigen::Success) { refuseAll(LevelHoldSkip::NoDataInformation); return out; }
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    out.lambda_max = ev(nr - 1);
    if (!(out.lambda_max > 0.0)) { refuseAll(LevelHoldSkip::NoDataInformation); return out; }
    int n_null = 0;
    for (Eigen::Index q = 0; q < nr; ++q)
        if (ev(q) < kAnchorLevelNullTol * out.lambda_max) ++n_null;
    out.n_null = n_null;
    if (n_null == 0) { refuseAll(LevelHoldSkip::NoNullDirection); return out; }
    const Eigen::MatrixXd Z = es.eigenvectors().leftCols(n_null);   // an orthonormal null basis

    for (System sys : want) {
        std::vector<Eigen::Index> in, outside;
        for (Eigen::Index c = 0; c < nr; ++c) {
            const System cs = (static_cast<std::size_t>(c) < col_sys.size())
                                  ? col_sys[static_cast<std::size_t>(c)] : System::Unknown;
            (cs == sys ? in : outside).push_back(c);
        }
        if (in.empty()) { out.refused.emplace_back(sys, LevelHoldSkip::NoColumns); continue; }
        // The one direction of the null space that vanishes outside the constellation's
        // columns: the right singular vector of the basis restricted to those columns whose
        // singular value is zero.  A column of the basis beyond that restriction's rank is a
        // zero singular value the decomposition does not list; it counts as one.
        Eigen::VectorXd alpha;
        if (outside.empty()) {
            if (n_null != 1) { out.refused.emplace_back(sys, LevelHoldSkip::NotUnique); continue; }
            alpha = Eigen::VectorXd::Ones(1);
        } else {
            Eigen::MatrixXd Zo(static_cast<Eigen::Index>(outside.size()), n_null);
            for (std::size_t k = 0; k < outside.size(); ++k) Zo.row(static_cast<Eigen::Index>(k)) = Z.row(outside[k]);
            const Eigen::JacobiSVD<Eigen::MatrixXd> svd(Zo, Eigen::ComputeFullV);
            const Eigen::VectorXd& sv = svd.singularValues();   // descending
            int n_small = n_null - static_cast<int>(sv.size());
            for (Eigen::Index k = 0; k < sv.size(); ++k)
                if (sv(k) < kAnchorLevelSplitTol) ++n_small;
            if (n_small == 0) { out.refused.emplace_back(sys, LevelHoldSkip::NoNullDirection); continue; }
            if (n_small > 1)  { out.refused.emplace_back(sys, LevelHoldSkip::NotUnique); continue; }
            alpha = svd.matrixV().col(n_null - 1);
        }
        Eigen::VectorXd c = Z * alpha;
        double off_max = 0.0;
        for (Eigen::Index k : outside) { off_max = std::max(off_max, std::abs(c(k))); c(k) = 0.0; }
        const double nrm = c.norm();
        if (!(nrm > 0.0) || !std::isfinite(nrm)) {
            out.refused.emplace_back(sys, LevelHoldSkip::NoNullDirection);
            continue;
        }
        c /= nrm;
        // The sign: the mean slant-ionosphere entry positive; the first non-zero entry of
        // the constellation's columns when it has no slant ionosphere.
        double iono_sum = 0.0;
        int n_iono = 0;
        for (Eigen::Index k : in)
            if (static_cast<std::size_t>(k) < col_iono.size() && col_iono[static_cast<std::size_t>(k)]) {
                iono_sum += c(k);
                ++n_iono;
            }
        bool flip = false;
        if (n_iono > 0 && std::abs(iono_sum) > 1e-12 * n_iono) {
            flip = iono_sum < 0.0;
        } else {
            for (Eigen::Index k : in)
                if (std::abs(c(k)) > 1e-12) { flip = c(k) < 0.0; break; }
        }
        if (flip) c = -c;
        out.found.push_back(LevelDirections::Entry{sys, std::move(c), off_max});
    }
    return out;
}

LevelHoldRow anchorLevelHoldRow(const Eigen::VectorXd& c, const std::vector<char>& carried,
                                const Eigen::VectorXd& x_prev, const Eigen::VectorXd& x_lin) {
    LevelHoldRow row;
    row.a_r = Eigen::VectorXd::Zero(c.size());
    for (Eigen::Index k = 0; k < c.size(); ++k) {
        if (static_cast<std::size_t>(k) >= carried.size() || !carried[static_cast<std::size_t>(k)]) continue;
        if (c(k) == 0.0) continue;
        row.a_r(k) = c(k);
        row.level_prev += c(k) * x_prev(k);
        row.level_lin  += c(k) * x_lin(k);
        ++row.n_cols;
    }
    row.y = row.level_prev - row.level_lin;
    return row;
}

const char* levelHoldSkipName(LevelHoldSkip s) {
    switch (s) {
        case LevelHoldSkip::None:              return "none";
        case LevelHoldSkip::NoColumns:         return "no_columns";
        case LevelHoldSkip::NoDataInformation: return "no_data";
        case LevelHoldSkip::NoNullDirection:   return "no_null_direction";
        case LevelHoldSkip::NotUnique:         return "not_unique";
        case LevelHoldSkip::NoCarriedColumn:   return "no_carried_column";
        case LevelHoldSkip::NonFinite:         return "non_finite";
    }
    return "?";
}

// ---- PASS B station step ----

StationStep networkAnchorLevelHoldRows(StationBuild& build) {
    // ---- From the station build ----
    const NetworkRunContext& ctx = build.ctx;
    const NetworkSetupResult& setup = build.setup;
    double pl_now_s = build.clock.pl_now_s;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool   hold_on  = ctx.knobs.pd_anchor_level_hold;
    const bool   hold_all = ctx.knobs.pd_anchor_level_hold_all;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool   relevel_free_on = ctx.knobs.relevel_free_on;
    const double pl_max_gap_s    = ctx.knobs.pl_max_gap_s;
    const double iono_blk_gap_s  = ctx.knobs.iono_blk_max_gap_s;
    const long long now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const net::ArcTable& arc_rows = ctx.entities.arcs;           // read only: the parallel produce
    const net::LosTable& los_rows = ctx.entities.los;            // read only
    const net::StationTable& sta_rows = ctx.entities.stations;   // read only
    const RelevelEpochState& relevel_ev = build.events.slip.relevel;
    auto& stations = setup.stations;
    // ---- End of the bindings ----

    if (!hold_on) return StationStep::Next;
    const StationParams& params = st.params;
    const Eigen::Index nr = R.A_r.cols();
    const Eigen::Index nd = std::min<Eigen::Index>(R.n_data_rows, R.A_r.rows());

    // The constellations the station anchors a family of this epoch (its pins), with the
    // family count: the tenure predicate of the hold's first form.
    std::map<System, int, net::ByLetter> n_fam;
    if (phase_datum_prioritized)
        for (const auto& [amb_key, target] : st.pinned_amb_targets) {
            (void)target;
            if (const auto id = net::satSignalKey(amb_key)) ++n_fam[id->sat.sys];
        }

    // The block's local columns: a level column (ambiguity, slant ionosphere, receiver code
    // bias) belongs to its constellation; every other column is outside every level.
    std::vector<System> col_sys(static_cast<std::size_t>(std::max<Eigen::Index>(nr, 0)), System::Unknown);
    std::vector<char>   col_iono(col_sys.size(), 0);
    std::vector<int>    col_state(col_sys.size(), -1);
    std::set<System, net::ByLetter> level_systems;
    for (const auto& [sidx, col] : h2local) {
        if (sidx < 0 || col < 0 || col >= nr) continue;
        if (static_cast<std::size_t>(sidx) >= params.slots.size()) continue;
        const ParamRef& ref = slotAt(params, sidx).ref;
        System sys = System::Unknown;
        switch (ref.kind) {
            case NetworkParamKind::Ambiguity:  sys = ref.sat.sys; break;
            case NetworkParamKind::SlantIono:  sys = ref.sat.sys; col_iono[static_cast<std::size_t>(col)] = 1; break;
            case NetworkParamKind::RxCodeBias: sys = ref.sys; break;
            default: break;
        }
        col_sys[static_cast<std::size_t>(col)]   = sys;
        col_state[static_cast<std::size_t>(col)] = sidx;
        if (sys != System::Unknown) level_systems.insert(sys);
    }

    // The candidates: the anchored constellations (H1), or every constellation with a level
    // column (H2).  An anchored constellation with no level column is recorded as refused.
    std::set<System, net::ByLetter> want_set;
    for (System s : level_systems) {
        const auto f = n_fam.find(s);
        if (hold_all || (f != n_fam.end() && f->second > 0)) want_set.insert(s);
    }
    for (const auto& [s, n] : n_fam)
        if (n > 0) want_set.insert(s);
    if (want_set.empty()) return StationStep::Next;
    const std::vector<System> want(want_set.begin(), want_set.end());

    const auto famOf = [&](System s) {
        const auto f = n_fam.find(s);
        return f == n_fam.end() ? 0 : f->second;
    };
    const auto recordSkip = [&](System s, LevelHoldSkip why, int n_null) {
        LevelHoldRecord rec;
        rec.sys = s;
        rec.n_fam = famOf(s);
        rec.tenure = rec.n_fam > 0;
        rec.skip = why;
        rec.n_null = n_null;
        R.level_hold.push_back(rec);
    };
    if (nr <= 0 || nd <= 0) {
        for (System s : want) recordSkip(s, nr <= 0 ? LevelHoldSkip::NoColumns : LevelHoldSkip::NoDataInformation, 0);
        return StationStep::Next;
    }

    const LevelDirections dirs = anchorLevelDirections(R.A_r.topRows(nd), R.w.head(nd), col_sys, col_iono, want);
    for (const auto& [s, why] : dirs.refused) recordSkip(s, why, dirs.n_null);

    const Eigen::VectorXd& x_lin = build.postUpdateState();
    for (const LevelDirections::Entry& e : dirs.found) {
        LevelHoldRecord rec;
        rec.sys = e.sys;
        rec.n_fam = famOf(e.sys);
        rec.tenure = rec.n_fam > 0;
        rec.n_null = dirs.n_null;
        rec.off_sys_max = e.off_sys_max;
        // The carried mask and values of the direction's columns.  A column is carried when
        // its carry entry is live this epoch: an ambiguity that is not gauge-pinned, not
        // freed, not pending a re-seed and not dropped this epoch, inside the leveling
        // horizon; a slant ionosphere inside its in-block horizon; a receiver code bias
        // inside its horizon.  Everything else is free in the row.
        std::vector<char> carried(col_sys.size(), 0);
        Eigen::VectorXd x_prev = Eigen::VectorXd::Zero(nr);
        Eigen::VectorXd x_lin_cols = Eigen::VectorXd::Zero(nr);
        for (Eigen::Index col = 0; col < nr; ++col) {
            if (e.c(col) == 0.0) continue;   // outside the constellation (exactly zero)
            const int sidx = col_state[static_cast<std::size_t>(col)];
            if (sidx < 0 || sidx >= x_lin.size()) { ++rec.n_excl_uncarried; continue; }
            x_lin_cols(col) = x_lin(sidx);
            const ParamRef& ref = slotAt(params, sidx).ref;
            const LocalCarry* lc = nullptr;
            double horizon_s = 0.0;
            switch (ref.kind) {
                case NetworkParamKind::Ambiguity: {
                    const std::string sat = net::satKey(ref.sat);
                    const std::string sig = net::signalKey(ref.signal);
                    if (networkIsGaugePinned(stations, phase_datum_prioritized,
                                             static_cast<std::size_t>(i), sat, sig)) {
                        ++rec.n_excl_pinned;   // the pins hold it
                        continue;
                    }
                    lc = net::get(arc_rows, ArcId{StationId{i}, ref.sat, ref.signal}, &net::ArcState::amb);
                    if (lc == nullptr) { ++rec.n_excl_uncarried; continue; }
                    const SatSignalId id{ref.sat, ref.signal};
                    const bool freed = pendingReseed(params, id)
                        || (relevel_free_on && relevel_ev.free_ambs.count({i, net::ambKey(id)}) > 0)
                        || (now_ep > 0 && lc->dropped_ep == now_ep);
                    if (freed) { ++rec.n_excl_dropped; continue; }
                    horizon_s = pl_max_gap_s;
                    break;
                }
                case NetworkParamKind::SlantIono:
                    lc = net::get(los_rows, LosId{StationId{i}, ref.sat}, &net::LosState::iono_blk);
                    horizon_s = iono_blk_gap_s;
                    break;
                case NetworkParamKind::RxCodeBias:
                    lc = net::get(sta_rows, StationId{i}, &net::StationState::signals, keyText(ref),
                                  &net::StationSignalState::rx_cb);
                    horizon_s = kRxCbMaxGapS;
                    break;
                default:
                    break;
            }
            if (lc == nullptr) { ++rec.n_excl_uncarried; continue; }
            if (!std::isfinite(lc->value_abs)
                || carryPastHorizon(pl_now_s, lc->last_seen_s, horizon_s)) {
                ++rec.n_excl_uncarried;
                continue;
            }
            carried[static_cast<std::size_t>(col)] = 1;
            x_prev(col) = lc->value_abs;
            switch (ref.kind) {
                case NetworkParamKind::Ambiguity: ++rec.n_amb;  break;
                case NetworkParamKind::SlantIono: ++rec.n_iono; break;
                default:                          ++rec.n_rxcb; break;
            }
        }
        const LevelHoldRow row = anchorLevelHoldRow(e.c, carried, x_prev, x_lin_cols);
        rec.n_cols = row.n_cols;
        rec.level_prev = row.level_prev;
        rec.level_lin  = row.level_lin;
        rec.y = row.y;
        if (row.n_cols == 0) { rec.skip = LevelHoldSkip::NoCarriedColumn; R.level_hold.push_back(rec); continue; }
        if (!std::isfinite(row.y)) { rec.skip = LevelHoldSkip::NonFinite; R.level_hold.push_back(rec); continue; }
        // The row, beside the pins: a_s = 0, a_r the restricted direction, the pins' weight,
        // aimed at the carried level.
        const Eigen::Index m0 = R.A_s.rows();
        networkGrowBlockRows(R.A_s, R.A_r, R.w, R.y, 1);
        R.A_r.row(m0) = row.a_r.transpose();
        R.w(m0) = kAnchorLevelHoldWeight;
        R.y(m0) = row.y;
        R.row_tags.push_back(RowTag{ContributionKind::AnchorLevelHold, SatId{}, SignalId{}});
        ++R.level_hold_rows;
        rec.held = true;
        R.level_hold.push_back(rec);
    }
    return StationStep::Next;
}

// ---- PASS B serial tally ----

namespace {

std::string joinNames(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& n : v) { if (!s.empty()) s += '+'; s += n; }
    return s.empty() ? std::string("-") : s;
}

}  // namespace

void networkAnchorLevelHoldTally(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                 const SyncedEpoch& se, const std::vector<NetStation>& stations,
                                 const std::vector<StationAssembly>& asm_res) {
    if (!ctx.knobs.pd_anchor_level_hold) return;
    net::StationTable& sta_rows = ctx.entities.stations;   // written
    AnchorLevelHoldCounters& C = ctx.counters;
    const long long now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    const GnssTime& t = se.time;
    ++C.alh_epochs;

    // This epoch's edges by constellation letter, for the handover event.
    std::map<char, std::vector<std::string>> ended, started;
    const auto nameOf = [&](std::size_t i) {
        return i < stations.size() ? stations[i].name : std::string("?");
    };
    for (std::size_t i = 0; i < asm_res.size(); ++i) {
        const StationAssembly& R = asm_res[i];
        const std::string name = nameOf(i);
        const StationId sid{static_cast<int>(i)};
        std::set<System, net::ByLetter> seen;
        std::set<char> held_sys;
        for (const LevelHoldRecord& r : R.level_hold) {
            seen.insert(r.sys);
            const char L = traitsOf(r.sys).letter;
            if (r.tenure) ++C.alh_tenure_records; else ++C.alh_all_records;
            C.alh_n_null_max  = std::max<long long>(C.alh_n_null_max, r.n_null);
            C.alh_off_sys_max = std::max(C.alh_off_sys_max, r.off_sys_max);
            if (r.held) {
                ++C.alh_rows;
                ++C.alh_rows_by_sys[L];
                held_sys.insert(L);
                C.alh_cols += r.n_cols; C.alh_cols_amb += r.n_amb;
                C.alh_cols_iono += r.n_iono; C.alh_cols_rxcb += r.n_rxcb;
                C.alh_excl_pinned += r.n_excl_pinned; C.alh_excl_dropped += r.n_excl_dropped;
                C.alh_excl_uncarried += r.n_excl_uncarried;
                if (std::abs(r.y) > C.alh_y_max) {
                    C.alh_y_max = std::abs(r.y);
                    C.alh_y_max_tow = t.tow;
                    C.alh_y_max_sys = L;
                    C.alh_y_max_station = name;
                }
            } else {
                switch (r.skip) {
                    case LevelHoldSkip::NoDataInformation: ++C.alh_skip_no_data;     break;
                    case LevelHoldSkip::NoNullDirection:   ++C.alh_skip_no_null;     break;
                    case LevelHoldSkip::NotUnique:         ++C.alh_skip_not_unique;  break;
                    case LevelHoldSkip::NoCarriedColumn:   ++C.alh_skip_no_carried;  break;
                    case LevelHoldSkip::NonFinite:         ++C.alh_skip_non_finite;  break;
                    case LevelHoldSkip::NoColumns:         ++C.alh_skip_no_columns;  break;
                    case LevelHoldSkip::None:                                        break;
                }
            }
            // The memo and the edges (standing invariant 7: the hold starts and ends as
            // events, with the identities and the counts).
            net::LevelHoldMemo& memo = sta_rows[sid].level_hold[r.sys];
            if (!memo.held && r.held) {
                const char* reason = !r.tenure ? "all_stations"
                                   : (memo.tenure ? "direction_found" : "tenure");
                memo.since_ep = now_ep;
                ++C.alh_starts;
                started[L].push_back(name);
                traces.emitEvent(t, "ANCHOR_LEVEL", "hold_start", "", name, r.level_prev,
                                 std::nullopt, "hold",
                                 fmt::format("sys={};reason={};n_fam={};n_cols={};n_amb={};"
                                             "n_iono={};n_rxcb={};excl_pinned={};excl_dropped={};"
                                             "excl_uncarried={};y_m={:.4f};n_null={}",
                                             L, reason, r.n_fam, r.n_cols, r.n_amb, r.n_iono,
                                             r.n_rxcb, r.n_excl_pinned, r.n_excl_dropped,
                                             r.n_excl_uncarried, r.y, r.n_null));
            } else if (memo.held && !r.held) {
                const char* reason = r.tenure ? levelHoldSkipName(r.skip) : "tenure_end";
                ++C.alh_ends;
                ended[L].push_back(name);
                traces.emitEvent(t, "ANCHOR_LEVEL", "hold_end", "", name, memo.level,
                                 std::nullopt, "release",
                                 fmt::format("sys={};reason={};n_fam={};held_ep={};level_m={:.4f}",
                                             L, reason, r.n_fam, now_ep - memo.since_ep,
                                             memo.level));
            }
            memo.held = r.held;
            memo.tenure = r.tenure;
            if (r.held) { memo.level = r.level_prev; ++memo.held_epochs; }
        }
        if (!held_sys.empty()) {
            ++C.alh_station_epochs;
            for (char L : held_sys) ++C.alh_station_epochs_by_sys[L];
        }
        // A constellation held last epoch that the step did not consider this epoch: the
        // station anchors nothing of it any more (tenure scope), or it left the assembly.
        const auto it = sta_rows.find(sid);
        if (it == sta_rows.end()) continue;
        for (auto& [sys, memo] : it->second.level_hold) {
            if (!memo.held || seen.count(sys)) continue;
            const char L = traitsOf(sys).letter;
            const char* reason = R.ok ? "tenure_end" : "absent";
            ++C.alh_ends;
            ended[L].push_back(name);
            traces.emitEvent(t, "ANCHOR_LEVEL", "hold_end", "", name, memo.level, std::nullopt,
                             "release",
                             fmt::format("sys={};reason={};n_fam=0;held_ep={};level_m={:.4f}",
                                         L, reason, now_ep - memo.since_ep, memo.level));
            memo.held = false;
            memo.tenure = false;
        }
    }
    // A handover: a constellation with at least one end and one start in the same epoch.
    for (const auto& [L, from] : ended) {
        const auto to = started.find(L);
        if (to == started.end()) continue;
        ++C.alh_handover_epochs;
        traces.emitEvent(t, "ANCHOR_LEVEL", "hold_handover", "", "",
                         static_cast<double>(to->second.size()), std::nullopt, "handover",
                         fmt::format("sys={};from={};to={}", L, joinNames(from),
                                     joinNames(to->second)));
    }
}

// ---- Run-end report ----

int networkAnchorLevelHoldRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const AnchorLevelHoldCounters& C = ctx.counters;

    if (!ctx.knobs.pd_anchor_level_hold) return 0;
    const char* scope = ctx.knobs.pd_anchor_level_hold_all
                            ? "every station with a level direction (H2)"
                            : "the anchors during their tenure (H1)";
    const auto bySys = [](const std::map<char, long long>& m) {
        std::string s;
        for (const auto& [L, n] : m) s += fmt::format("{}{} {}", s.empty() ? "" : ", ", L, n);
        return s.empty() ? std::string("none") : s;
    };
    const auto per = [&](long long n) { return C.alh_rows > 0 ? double(n) / double(C.alh_rows) : 0.0; };
    if (C.alh_rows == 0) {
        log->warn("Network (ALD anchor-level hold): ARMED AND INERT — scope {}, {} epoch(s), "
                  "{} tenure record(s), 0 rows built (no data {}, no null direction {}, not "
                  "unique {}, no carried column {}, non-finite {}, no columns {}).  A hold that "
                  "never acts holds nothing (standing invariant 4).",
                  scope, C.alh_epochs, C.alh_tenure_records, C.alh_skip_no_data,
                  C.alh_skip_no_null, C.alh_skip_not_unique, C.alh_skip_no_carried,
                  C.alh_skip_non_finite, C.alh_skip_no_columns);
        return 0;
    }
    log->info("Network (ALD anchor-level hold): scope {}; {} row(s) over {} station-epoch(s) of "
              "{} epoch(s) (rows by constellation: {}; station-epochs: {}); per row {:.1f} "
              "carried column(s) ({:.1f} ambiguities, {:.1f} slant ionospheres, {:.1f} receiver "
              "code biases), left free {:.1f} pinned, {:.1f} freed or dropped, {:.1f} uncarried.",
              scope, C.alh_rows, C.alh_station_epochs, C.alh_epochs, bySys(C.alh_rows_by_sys),
              bySys(C.alh_station_epochs_by_sys), per(C.alh_cols), per(C.alh_cols_amb),
              per(C.alh_cols_iono), per(C.alh_cols_rxcb), per(C.alh_excl_pinned),
              per(C.alh_excl_dropped), per(C.alh_excl_uncarried));
    const long long records = C.alh_tenure_records + C.alh_all_records;
    log->info("Network (ALD anchor-level hold): edges {} start(s), {} end(s), {} handover "
              "epoch(s); records with no row {} of {} ({} with tenure, {} all-stations): no data "
              "{}, no null direction {}, not unique {}, no carried column {}, non-finite {}, no "
              "columns {}; largest |innovation| {:.4f} m at {} {} TOW {:.1f}; largest data-only "
              "null dimension {}; largest off-constellation loading {:.2e} (the level carries no "
              "clock, inter-system bias or wet-delay loading when this is near zero).",
              C.alh_starts, C.alh_ends, C.alh_handover_epochs, records - C.alh_rows, records,
              C.alh_tenure_records, C.alh_all_records, C.alh_skip_no_data, C.alh_skip_no_null,
              C.alh_skip_not_unique, C.alh_skip_no_carried, C.alh_skip_non_finite,
              C.alh_skip_no_columns, C.alh_y_max,
              C.alh_y_max_station.empty() ? std::string("-") : C.alh_y_max_station,
              C.alh_y_max_sys, C.alh_y_max_tow, C.alh_n_null_max, C.alh_off_sys_max);
    return 0;
}

} // namespace GnssProcessor
