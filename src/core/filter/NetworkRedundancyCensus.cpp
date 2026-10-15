/**
 * @file NetworkRedundancyCensus.cpp
 * @brief Redundancy and testability census.  The design argument is in
 *        NetworkRedundancyCensus.hpp (docs/SPEC_redundancy_census.md).
 *
 * Measure-only.  Nothing here writes a weight, a state or a decision; the
 * translation unit reads the solved system and emits counters and events.
 *
 * ## The closure check (standing invariant #4, applied to the statistics)
 *
 * With @f$h_{ii} = w_i a_i^\top \mathrm{Cov}\,a_i = 1 - r_i@f$ the whitened
 * hat-matrix diagonal, and @f$\mathrm{Cov} = (A^\top W A + P)^{-1}@f$ with
 * @f$P \succeq 0@f$ the folded prior information,
 *
 *   @f[ \sum_i h_{ii} = \mathrm{tr}\big((A^\top W A)(A^\top W A + P)^{-1}\big)
 *       = n_\text{free} - \mathrm{tr}\big(P(A^\top W A+P)^{-1}\big)
 *       \le n_\text{free}. @f]
 *
 * Every @f$h_{ii} \ge 0@f$ (a quadratic form in a PSD covariance), so the bound
 * holds on any subset of rows, which makes it usable on a targeted single-station
 * sweep.  The census reports the slack @f$n_\text{free} - \sum h_{ii}@f$ and
 * treats a negative slack as a defect in the covariance or leverage path, not as
 * a finding about the data.
 *
 * Two caveats:
 *  - the kernel clamps @f$r_i@f$ to [0,1] (a non-finite @f$r_i@f$ becomes 0), so
 *    an over-unity leverage is reported as @f$h = 1@f$, and rows with
 *    @f$r_i \le 0@f$ are counted separately (@c redun_r_clamped).  Capping
 *    @f$h@f$ at 1 can only lower @f$\sum h_{ii}@f$, so it never produces a
 *    negative slack by itself;
 *  - @f$n_\text{free}@f$ counts the shared columns that are not exact gauges,
 *    measured directly as the non-zero columns of the datum-frame covariance, so
 *    both the datum pins and the ensemble phase gauges are excluded without this
 *    file having to know which mechanism zeroed them.
 */

#include "filter/NetworkRedundancyCensus.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkBlockBaarda.hpp" // BlockFormal LOS-block rider on this sweep (SPEC_redundancy_census)
#include "filter/NetworkCongruence.hpp" // Station congruence rider on this sweep (SPEC_redundancy_census)
#include "filter/NetworkMdb.hpp"        // Baarda MDB rider on the swept r_i (SPEC_redundancy_census)
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"

// The run-end reports at the end of this file (card NOM step TD1b):
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "app/NetworkSetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

// ===========================================================================
//  Testable pieces (no run state, no I/O)
// ===========================================================================

StationPrefitCensus stationPrefitCensus(const std::vector<NetRowClass>& cls,
                                        const StationAssembly& R,
                                        int min_pool_rows) {
    StationPrefitCensus c;
    const Eigen::Index m = std::min<Eigen::Index>(
        R.y.size(), static_cast<Eigen::Index>(cls.size()));
    if (m <= 0) return c;
    c.s.assign(static_cast<std::size_t>(m), std::numeric_limits<double>::quiet_NaN());

    // Pool 0/1/2 = code/phase/other.  iono_sh_row (data class index 3) carries no
    // receiver clock, so it is standardized raw and joins no pool.
    std::vector<double> pool[3];
    for (Eigen::Index row = 0; row < m; ++row) {
        const int d = dataClassIndex(cls[static_cast<std::size_t>(row)]);
        if (d >= 0 && d < 3 && std::isfinite(R.y(row))) pool[d].push_back(R.y(row));
    }
    bool pool_ok[3] = {false, false, false};
    for (int p = 0; p < 3; ++p) {
        c.n[p] = static_cast<int>(pool[p].size());
        if (c.n[p] == 0) continue;
        if (c.n[p] < min_pool_rows) { ++c.n_pool_skipped; continue; }
        std::nth_element(pool[p].begin(),
                         pool[p].begin() + static_cast<std::ptrdiff_t>(pool[p].size() / 2),
                         pool[p].end());
        c.med[p] = pool[p][pool[p].size() / 2];
        pool_ok[p] = true;
    }
    for (Eigen::Index row = 0; row < m; ++row) {
        const NetRowClass k = cls[static_cast<std::size_t>(row)];
        if (!isDataRowClass(k)) continue;                 // pins / coupling excluded
        const int d = dataClassIndex(k);
        const bool centred = (d >= 0 && d < 3);
        if (centred && !pool_ok[d]) continue;             // pool skipped this epoch
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        if (!(w > 0.0) || !std::isfinite(R.y(row))) continue;
        const double dy = centred ? (R.y(row) - c.med[d]) : R.y(row);
        const double s  = std::abs(dy) * std::sqrt(w);
        if (!std::isfinite(s)) continue;
        c.s[static_cast<std::size_t>(row)] = s;
        ++c.n_data_rows;
        if (s > c.max_s) { c.max_s = s; c.max_row = row; }
    }
    return c;
}

std::vector<Eigen::Index> unobservableLocalColumns(const Eigen::VectorXd& pivots,
                                                   double eps) {
    std::vector<Eigen::Index> out;
    const Eigen::Index n = pivots.size();
    if (n <= 0 || !(eps > 0.0)) return out;
    // Median over the finite positive pivots only: a non-positive pivot is already
    // a finding and is kept out of the reference scale so it cannot lower it.
    std::vector<double> good;
    good.reserve(static_cast<std::size_t>(n));
    for (Eigen::Index k = 0; k < n; ++k)
        if (std::isfinite(pivots(k)) && pivots(k) > 0.0) good.push_back(pivots(k));
    if (good.empty()) {                    // every column degenerate: flag them all
        for (Eigen::Index k = 0; k < n; ++k) out.push_back(k);
        return out;
    }
    std::nth_element(good.begin(),
                     good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2),
                     good.end());
    const double thresh = eps * good[good.size() / 2];
    for (Eigen::Index k = 0; k < n; ++k) {
        const double d = pivots(k);
        if (!std::isfinite(d) || d <= 0.0 || d < thresh) out.push_back(k);
    }
    return out;
}

// ===========================================================================
//  File-local helpers
// ===========================================================================

namespace {

/// Sentinel StateManager returns for "this state does not exist".
constexpr std::size_t kNoState = std::numeric_limits<std::size_t>::max();

/**
 * @brief Local A_r column → human state label, via @c h2local and the station's
 *        StateManager key maps.
 *
 * @c h2local maps H (state-vector) column → A_r column, so the label pass walks
 * the StateManager's key maps forward and installs each key on the A_r column
 * its state index maps to.  A column no map claims gets the positional
 * fallback @c "loc<col>" (see localLabel): the census still counts and reports
 * a column it cannot name.
 */
std::map<Eigen::Index, std::string> localColumnLabels(const StationAssembly& R,
                                                      const StateManager* sm) {
    std::map<Eigen::Index, std::string> out;
    if (sm == nullptr) return out;
    auto put = [&](std::size_t state_idx, const std::string& label) {
        if (state_idx == kNoState) return;
        const auto hit = R.h2local.find(static_cast<int>(state_idx));
        if (hit == R.h2local.end() || hit->second < 0) return;
        out.emplace(static_cast<Eigen::Index>(hit->second), label);
    };
    for (const auto& kv : sm->getIonosphereStates())        put(kv.second, "iono:" + kv.first);
    for (const auto& kv : sm->getIonosphereRateStates())    put(kv.second, "ionorate:" + kv.first);
    for (const auto& kv : sm->getAmbiguities())             put(kv.second, "amb:" + kv.first);
    for (const auto& kv : sm->getCodeBiasStates())          put(kv.second, "rxcb:" + kv.first);
    for (const auto& kv : sm->getReceiverPhaseBiasStates()) put(kv.second, "rpb:" + kv.first);
    put(sm->getTropoIndex(),              "zwd");
    put(sm->getTropoGradientNorthIndex(), "grad_n");
    put(sm->getTropoGradientEastIndex(),  "grad_e");
    put(sm->getClockIndex(),              "rx_clock");
    put(sm->getClockRateIndex(),          "rx_clock_rate");
    put(sm->getPhaseClockIndex(),         "rx_phase_clock");
    return out;
}

/// Label of local column @p col, or the positional fallback.
std::string localLabel(const std::map<Eigen::Index, std::string>& labels,
                       Eigen::Index col) {
    const auto hit = labels.find(col);
    return (hit != labels.end()) ? hit->second : ("loc" + std::to_string(col));
}

}  // namespace

// ===========================================================================
//  The seam
// ===========================================================================

void networkRedundancyCensus(NetworkRun& run, const NetworkEpoch& nep) {
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
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const NetworkEstimator::Datum& plan_datum = nep.solve.plan.shared_datum;
    const std::vector<StationAssembly>& asm_res = nep.assembly.asm_res;
    const std::vector<int>& est_index_of = nep.assembly.est_index_of;
    const Eigen::VectorXd& x_s = nep.solve.x_s;
    // ---- End of the bindings ----
    if (!ctx.knobs.redun_census_on) return;   // the driver's gate (card NOM3 step EP2)
    // Alias preamble (seam contract): the body reads the driver's names.
    const auto& nc      = config_.getNetworkConfig();
    auto& carry         = ctx.carry;
    auto& counters      = ctx.counters;
    const double eps        = nc.redundancy_eps;
    const double hot_sigma  = nc.redundancy_hot_sigma;
    const double pivot_eps  = nc.redundancy_pivot_eps;
    const double escal_sig  = nc.redundancy_escalate_sigma;
    const int    stride     = nc.redundancy_census_stride;
    const int    holdoff    = std::max(0, nc.redundancy_event_holdoff_ep);
    // MDB rider on this sweep's r_i (docs/SPEC_redundancy_census.md MDB-INC-0).
    // Swept-row MDB uses the census's stride and escalation unchanged: no extra
    // sweep, no second covariance product, one division per swept row.
    const bool   mdb_on     = ctx.knobs.mdb_on && ctx.knobs.mdb_from_census;
    const double mdb_delta0 = baardaDelta0(config_.dataSnoopingCritical());
    // BlockFormal (block-Baarda) rider on this sweep's swept stations
    // (docs/SPEC_redundancy_census.md BB-INC-1).  It inherits the stride, the
    // targeted escalation and the trip-forced full sweep; SPEC §4 requires no new
    // scheduler, which is why it runs here rather than in a seam of its own.  It
    // needs the post-fit residual, so an empty x_s disarms it; run on the prefit
    // innovation it would produce a plausible ladder of the wrong quantity.
    const bool bb_on = ctx.knobs.bb_on && ctx.carry.bb.enabled() && x_s.size() > 0;
    // Congruence rider, the second rider on this sweep: the coordinate-fault screen
    // in position space (docs/SPEC_redundancy_census.md CG-INC-1).  Same host, same
    // stride, same post-fit residual (formed once below and passed to both riders),
    // and the same empty-x_s disarm: run on the prefit innovation it would produce
    // a plausible delta_hat of the wrong quantity.
    const bool cg_on = ctx.knobs.cg_on && ctx.carry.cg.enabled() && x_s.size() > 0;

    const auto t0 = std::chrono::steady_clock::now();
    ++counters.redun_passes;
    const long long ep = carry.redun_epochs++;   // stride phase; 0 on the first pass

    // ---- the epoch's scalars stay blank unless this pass fills them -------
    carry.redun_ep_tow           = -1.0;
    carry.ep_redun_rows          = 0;
    carry.ep_uncheckable_rows    = 0;
    carry.ep_uncheckable_max_s   = 0.0;
    carry.ep_unobservable_local  = 0;

    const Eigen::Index n_shared = est.numShared();
    if (n_shared <= 0) return;
    const Eigen::MatrixXd& cov_raw = est.sharedCovariance();
    if (cov_raw.rows() != n_shared || cov_raw.cols() != n_shared) {
        if (!carry.redun_cov_warned) {
            carry.redun_cov_warned = true;
            log->warn("Network (redundancy census): ARMED but the shared covariance is "
                      "{}x{} against {} shared columns at TOW {:.1f} — no solve to read. "
                      "Every sweep this run will be skipped (invariant #4).",
                      cov_raw.rows(), cov_raw.cols(), n_shared, se.time.tow);
        }
        return;
    }

    // ---- which stations get the costly sweep this epoch -------------------
    // A flight-recorder epoch always forces a full sweep, so the attribution dump
    // and the census describe the same epoch and a discharge can be attributed in
    // the run where it happens.  Two trip channels, both only read here (each dump
    // seam runs later in the same SB_FRAMEDIAG block and consumes its own):
    //   * an armed rhs_attrib_tow window that has not yet been spent, and
    //   * a trip raised earlier this epoch by a QC trigger site
    //     (SPEC_prefit_census_observability INC-5; "" = no trip).
    const bool trip = (nc.rhs_attrib_tow >= 0.0
                       && ctx.carry.rhs_attrib_done < std::max(1, nc.rhs_attrib_n_ep)
                       && se.time.tow >= nc.rhs_attrib_tow - 0.25)
                   || !ctx.carry.rhs_trip_pending.empty();
    const bool sweep_all = censusSweepsThisEpoch(ep, stride, trip);

    std::set<std::size_t> sweep_set;
    if (sweep_all) {
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i)
            if (i < est_index_of.size() && est_index_of[i] >= 0) sweep_set.insert(i);
    } else {
        // Targeted: the stations the cheap tier flagged last epoch.
        for (std::size_t i : carry.redun_escalate_pending)
            if (i < asm_res.size() && i < est_index_of.size() && est_index_of[i] >= 0)
                sweep_set.insert(i);
        if (!sweep_set.empty()) counters.redun_escalations +=
            static_cast<long long>(sweep_set.size());
    }
    carry.redun_escalate_pending.clear();

    // The cheap tier walks every station when escalation is armed, because it
    // decides which stations are swept next epoch; otherwise it walks only the
    // stations being swept, where the cross-flag needs the magnitude half.
    std::set<std::size_t> census_set = sweep_set;
    if (escal_sig > 0.0)
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i)
            if (i < est_index_of.size() && est_index_of[i] >= 0) census_set.insert(i);
    if (census_set.empty()) return;

    // Datum-frame shared covariance, built once per epoch (the per-station wrapper
    // would copy the whole n_shared² block per call: 63 × 450² doubles on the FT63
    // tier).  Exact gauges are zeroed inside; n_free counts the columns left.
    Eigen::MatrixXd cov_free;
    try { cov_free = est.datumFrameSharedCov(plan_datum); }
    catch (...) { return; }
    Eigen::Index n_shared_free = 0;
    for (Eigen::Index c = 0; c < n_shared; ++c)
        if (!cov_free.col(c).isZero(0.0)) ++n_shared_free;

    const NetworkNormalEquations& kernel = est.normals();

    // ---- per-epoch accumulators -------------------------------------------
    long long ep_rows_data = 0, ep_unchk = 0, ep_unobs = 0, ep_clamped = 0;
    double    ep_unchk_max_s = 0.0;
    double    trace_h = 0.0;
    Eigen::Index n_local_swept = 0;
    bool      swept_any = false;

    auto emit = [&](const char* event, const std::string& sat,
                    const std::string& station, std::optional<double> statistic,
                    std::optional<double> threshold, const std::string& detail) {
        traces.emitEvent(se.time, "REDUNDANCY", event, sat, station, statistic,
                         threshold, "observe", detail);
        ++counters.redun_events;
    };

    for (std::size_t i : census_set) {
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n_shared || R.y.size() == 0) continue;
        const std::size_t slot = static_cast<std::size_t>(est_index_of[i]);
        const std::string& sname = stations[i].name;

        // ---- cheap tier: classes and the centred magnitude metric ----------
        // Each row's class and satellite come from its contribution tag (card NOM
        // step N2b, filter/NetworkRowClassify).  An SH tie row is filed under no
        // satellite: a whitened tie row spans the slant ionospheres of several lines of
        // sight, so it joins no per-satellite cell, block or bin (the block-Baarda census
        // counts it as an unnamed row), as before the tags, when its only shared columns
        // were the SH coefficients.
        const Eigen::Index m = R.y.size();
        std::vector<NetRowClass> cls(static_cast<std::size_t>(m));
        std::vector<std::string> sat(static_cast<std::size_t>(m));
        for (Eigen::Index row = 0; row < m; ++row) {
            const std::size_t u = static_cast<std::size_t>(row);
            cls[u] = classifyRow(R, row, &sat[u]);
            if (cls[u] == NetRowClass::IonoShRow) sat[u].clear();
        }
        const StationPrefitCensus pc = stationPrefitCensus(cls, R);
        counters.redun_pool_skips += pc.n_pool_skipped;

        // Escalation latch: this station's largest centred deviation decides
        // whether it gets a targeted sweep next epoch.  The latch is set even when
        // the station is swept this epoch: a park persists, and the next epoch's
        // sweep confirms it.
        if (escal_sig > 0.0 && pc.max_s > escal_sig) {
            carry.redun_escalate_pending.insert(i);
            ++counters.redun_escalate_flagged;   // counts flags, not sweeps taken
        }

        if (sweep_set.find(i) == sweep_set.end()) continue;   // cheap tier only

        // ---- costly tier: per-row redundancy ------------------------------
        Eigen::VectorXd r;
        try { r = kernel.rowRedundancy(slot, R.A_s, R.A_r, R.w, cov_free); }
        catch (...) { continue; }
        if (r.size() != m) continue;
        swept_any = true;
        ++counters.redun_station_sweeps;
        n_local_swept += kernel.numLocal(slot);

        // The closure sums over every row (pin and coupling rows carry leverage
        // too); the census statistics below cover data rows only.
        for (Eigen::Index row = 0; row < m; ++row) {
            const double h = 1.0 - r(row);
            if (std::isfinite(h)) trace_h += h;
            if (r(row) <= 0.0) ++ep_clamped;
        }
        counters.redun_rows_all += static_cast<long long>(m);

        // The MDB rider's feed for this station.  It is constructed inside the
        // swept branch so it sees the same rows as the census statistics below;
        // the run-end uncheckable join relies on that shared domain.
        std::optional<NetworkMdbStationFeed> mdb_feed;
        if (mdb_on)
            mdb_feed.emplace(ctx.carry.mdb, se.time.tow, mdb_delta0,
                             ctx.knobs.mdb_event_m, sname, NetMdbSource::Census, eps);

        double sta_sum_r = 0.0;
        long long sta_rows = 0, sta_unchk = 0;
        for (Eigen::Index row = 0; row < m; ++row) {
            const NetRowClass k = cls[static_cast<std::size_t>(row)];
            if (!isDataRowClass(k)) continue;
            ++sta_rows;
            sta_sum_r += r(row);
            // One division per swept data row.  Fed before every usability screen
            // below, so the rider's independent recount of "data rows" and "rows
            // below eps" covers the set the census counts; the join is a check
            // only if both walk the same domain.
            if (mdb_on)
                mdb_feed->add(sat[static_cast<std::size_t>(row)], k,
                              (row < R.w.size()) ? R.w(row) : 1.0, r(row));
            const double s = pc.s[static_cast<std::size_t>(row)];
            if (!(r(row) < eps)) continue;
            ++sta_unchk;                     // uncheckable: counted, never acted on
            if (!std::isfinite(s)) continue;
            if (s > ep_unchk_max_s) ep_unchk_max_s = s;
            // ---- the cross-flag -------------------------------------------
            if (isUncheckableHotCell(r(row), s, eps, hot_sigma)) {
                ++counters.redun_uncheckable_hot;
                if (s > counters.redun_max_hot_s) {
                    counters.redun_max_hot_s = s;
                    counters.redun_max_hot_id = fmt::format(
                        "{}/{}/{} @tow {:.1f}", sname,
                        sat[static_cast<std::size_t>(row)].empty()
                            ? "?" : sat[static_cast<std::size_t>(row)],
                        rowClassName(k), se.time.tow);
                }
                // Rate limit per (station, satellite, class), holdoff counted in
                // census passes: a parked cell logs its identity periodically
                // instead of every epoch (120 times an hour at 30 s cadence).
                // The line of sight's holdoff clock of the class (an empty satellite is the
                // station's own row, SatId{}).
                const std::optional<LosId> hid = net::losOf(
                    ctx.entities, static_cast<int>(i), sat[static_cast<std::size_t>(row)]);
                const std::string cls = rowClassName(k);
                const long long* lit =
                    hid ? net::get(ctx.entities.los, *hid, &net::LosState::events, cls,
                                   &net::LosEventState::redun_event_last)
                        : nullptr;
                if (lit == nullptr || (ep - *lit) >= holdoff) {
                    if (hid)
                        net::put(ctx.entities.los, *hid, &net::LosState::events, cls,
                                 &net::LosEventState::redun_event_last) = ep;
                    emit("uncheckable_hot_cell", sat[static_cast<std::size_t>(row)],
                         sname, s, hot_sigma,
                         fmt::format("class={};r={:.3e};eps={:g};y={:.4e};w={:.4e};row={}",
                                     rowClassName(k), r(row), eps, R.y(row),
                                     (row < R.w.size()) ? R.w(row) : 1.0, row));
                }
            } else if (s > counters.redun_max_cold_s) {
                // Control for the fallback directive: an uncheckable row that is
                // not hot is counted and never emitted as an event (a healthy
                // single-frequency riser falls here).  Keeping the largest such s
                // makes "counted, and none was hot" checkable at run end.
                counters.redun_max_cold_s = s;
                counters.redun_max_cold_id = fmt::format(
                    "{}/{}/{} @tow {:.1f}", sname,
                    sat[static_cast<std::size_t>(row)].empty()
                        ? "?" : sat[static_cast<std::size_t>(row)],
                    rowClassName(k), se.time.tow);
            }
        }
        if (mdb_on) mdb_feed->finish();   // commit this station's event candidates
        ep_rows_data += sta_rows;
        ep_unchk     += sta_unchk;
        counters.redun_rows        += sta_rows;
        counters.redun_uncheckable += sta_unchk;
        counters.redun_mass        += sta_sum_r;

        // ---- BlockFormal rider: the (station, satellite) LOS blocks ---------
        // docs/SPEC_redundancy_census.md BB-INC-1.  It uses the station, rows,
        // hoisted covariance and kernel slot the row census just used, so the
        // added cost is one back-substitution, two GEMVs and a b×b solve per
        // block, with b ≤ 8.
        if (bb_on || cg_on) {
            // The post-fit residual.  The census statistic itself does not need it
            // (r_i depends on the geometry alone), but T_B does, and
            // StationAssembly::y is the prefit innovation.  The estimator wrapper
            // is used instead of the raw kernel method so this cost is booked in
            // the D0 cov_consumers_ms line and SPEC §5's budget stays checkable.
            //
            // Both riders read the residual and it is formed once per station: the
            // congruence screen (docs/SPEC_redundancy_census.md) needs the same
            // y − A_s x_s − A_r x_r over the same station, and a second
            // reconstructStation call would double the most expensive call in
            // either rider's budget.
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(slot, x_s); }
            catch (...) { x_r.resize(0); }
            if (x_r.size() != R.A_r.cols()) {
                if (bb_on) ++ctx.counters.bb.no_resid;
                if (cg_on) ++ctx.counters.cg.no_resid;
            } else {
                const Eigen::VectorXd resid = R.y - R.A_s * x_s - R.A_r * x_r;
                // Source of the RESPONDED label, built once per station.  It is an
                // estimator-side signal (RAIM or slip refloat has marked the arc
                // for reseeding this epoch), kept separate from the raw slip-QC
                // verdict that INC-2 corroborates against: if the label and the
                // corroboration shared a source, SPEC §8.2's positive control (the
                // RESPONDED ladder against the CLEAN one) would be circular and
                // could not fail.
                //
                // This depends on seam order.  SB_LEVELING's carry harvest clears
                // the pending-reseed set (NetworkLevelingCarry.cpp,
                // clearPendingReseed), and SB_RAIM then marks this epoch's
                // refloats, so at SB_FRAMEDIAG the set holds the arcs an
                // estimator-side mechanism responded to in this epoch, which is
                // what the label means.  If this feed moved above SB_RAIM, every
                // block would read CLEAN without any warning.
                const std::size_t bb_hits_before = ctx.carry.bb_hits.size();
                if (bb_on) {
                    std::set<std::string> responded;
                    if (const StateManager* sm = stations[i].sm.get()) {
                        for (const auto& kv : sm->getAmbiguities()) {
                            if (!sm->isAmbiguityPendingReseed(kv.first)) continue;
                            if (const auto parts = StateKeys::parseAmbiguityKey(kv.first))
                                responded.insert(parts->satellite);
                        }
                    }
                    networkBlockBaardaSweepStation(
                        ctx.carry.bb, kernel, slot, static_cast<int>(i), R, cls, sat,
                        resid, cov_free, responded, ctx.carry.last_datum_changed,
                        ctx.counters.bb, ctx.carry.bb_hits);
                }

                // ---- Congruence rider: the station's coordinates -------------
                // docs/SPEC_redundancy_census.md CG-INC-1.  Runs after the block
                // sweep: INC-3's separability join needs to know whether the
                // per-LOS tier flagged anything at this station in this epoch, and
                // the growth of bb_hits across the block sweep is that verdict.
                // Reading ctx.carry.bb_hits later would couple the congruence
                // screen to the block census's emission seam, which drains it.
                if (cg_on)
                    networkCongruenceSweepStation(
                        ctx.carry.cg, kernel, slot, static_cast<int>(i), R, cls,
                        resid, cov_free, mdb_delta0, ctx.carry.last_datum_changed,
                        /*block_known=*/bb_on,
                        /*block_flagged=*/bb_on
                            && ctx.carry.bb_hits.size() > bb_hits_before,
                        ctx.counters.cg, ctx.carry.cg_hits);
            }
        }

        // ---- unobservable local columns (INC-2) ---------------------------
        Eigen::VectorXd piv;
        try { piv = kernel.stationLocalPivots(slot); }
        catch (...) { piv.resize(0); }
        if (piv.size() > 0) {
            counters.redun_pivot_cols += static_cast<long long>(piv.size());
            const std::vector<Eigen::Index> bad = unobservableLocalColumns(piv, pivot_eps);
            ep_unobs += static_cast<long long>(bad.size());
            counters.redun_pivot_flagged += static_cast<long long>(bad.size());
            std::set<std::string> now;
            std::map<Eigen::Index, std::string> labels;
            if (!bad.empty()) labels = localColumnLabels(R, stations[i].sm.get());
            for (Eigen::Index col : bad) now.insert(localLabel(labels, col));
            // Both edges (invariant #7): entry into the set (the decoupling edge)
            // emits an event with the column's identity; exit (the re-coupling
            // edge) is counted.  The per-station set is rebuilt each sweep, so a
            // column that recovers emits again if it becomes unobservable again.
            std::set<std::string>& prev =
                net::put(ctx.entities.stations, StationId{static_cast<int>(i)},
                         &net::StationState::redun_unobs_active);
            for (const std::string& kkey : now) {
                if (prev.find(kkey) != prev.end()) continue;
                ++counters.redun_unobs_entered;
                emit("unobservable_local", "", sname, std::nullopt, pivot_eps,
                     fmt::format("key={};n_local={}", kkey, piv.size()));
            }
            for (const std::string& kkey : prev)
                if (now.find(kkey) == now.end()) ++counters.redun_unobs_cleared;
            prev = std::move(now);
        }
    }

    if (swept_any) {
        ++counters.redun_sweeps;
        if (sweep_all) ++counters.redun_sweeps_full;
        else           ++counters.redun_sweeps_targeted;
        if (trip)      ++counters.redun_sweeps_trip;
        counters.redun_r_clamped += ep_clamped;

        // Closure: trace(H) over the swept rows must not exceed the free
        // parameter count they can possibly span.  See the file banner.
        const double n_free = static_cast<double>(n_shared_free)
                            + static_cast<double>(n_local_swept);
        const double slack = n_free - trace_h;
        if (!counters.redun_closure_any || slack < counters.redun_closure_min_slack) {
            counters.redun_closure_any = true;
            counters.redun_closure_min_slack = slack;
        }
        if (slack < -1e-6 * std::max(1.0, n_free)) {
            ++counters.redun_closure_neg;
            if (!carry.redun_closure_warned) {
                carry.redun_closure_warned = true;
                log->warn("Network (redundancy census): CLOSURE VIOLATED at TOW {:.1f} — "
                          "trace(H) = {:.4f} over {} swept row(s) exceeds the {:.0f} free "
                          "parameter(s) they span (slack {:.3e}).  The redundancy numbers "
                          "this run are UNTRUSTED: the covariance or the row stack does "
                          "not describe the system that was solved.",
                          se.time.tow, trace_h, counters.redun_rows_all, n_free, slack);
            }
        }

        // The epoch-status scalars (L1), valid for this epoch only: the writer
        // compares the stamp, so a non-sweep epoch leaves the columns blank
        // instead of repeating an older sweep's numbers.
        carry.redun_ep_tow          = se.time.tow;
        carry.ep_redun_rows         = ep_rows_data;
        carry.ep_uncheckable_rows   = ep_unchk;
        carry.ep_uncheckable_max_s  = ep_unchk_max_s;
        carry.ep_unobservable_local = ep_unobs;
        // The per-epoch worst identity is not carried: a hot cell reaches
        // network_events.csv with its full identity and the run's worst is in
        // the counters, so a third copy would be state nothing reads.
    }

    counters.redun_wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the redundancy and testability census.
int networkRedundancyCensusRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkConfig& nc = v.config.getNetworkConfig();

    // --- Redundancy / testability census run-end telemetry --------------------
    // (docs/SPEC_redundancy_census.md).  The census claims that the cross-flag has
    // essentially zero false-positive surface, and a reader can check that only with
    // the denominators next to the findings (invariant #4): passes, sweeps, rows,
    // uncheckable rows, and uncheckable rows that were also hot.  Zero hot cells over
    // zero swept rows says nothing about the network; zero hot cells over 400k swept
    // rows with 2k uncheckable is a real negative result.  The lines follow that order.
    if (!ctx.knobs.redun_census_on) return 0;
    const auto& q = ctx.counters;
    log->info("Network (redundancy census): {} seam pass(es), {} sweep epoch(s) "
              "({} full / {} targeted, {} forced by a flight-recorder trip), {} "
              "station-sweep(s), {} escalation(s) taken; {:.1f} ms total wall "
              "({:.2f} ms per station-sweep).",
              q.redun_passes, q.redun_sweeps, q.redun_sweeps_full,
              q.redun_sweeps_targeted, q.redun_sweeps_trip, q.redun_station_sweeps,
              q.redun_escalations, q.redun_wall_ms,
              (q.redun_station_sweeps > 0)
                  ? q.redun_wall_ms / static_cast<double>(q.redun_station_sweeps)
                  : 0.0);
    log->info("Network (redundancy census INC-1): {} DATA row(s) examined ({} rows "
              "total incl. pins/coupling), redundancy mass {:.1f} (mean r = {:.4f}); "
              "{} UNCHECKABLE row(s) at r < {:g} ({:.3f}% of data rows), of which {} "
              "ALSO exceeded s > {:g} — the CROSS-FLAG.  {} event(s) emitted after "
              "the {}-pass per-cell holdoff.",
              q.redun_rows, q.redun_rows_all, q.redun_mass,
              (q.redun_rows > 0) ? q.redun_mass / static_cast<double>(q.redun_rows)
                                 : 0.0,
              q.redun_uncheckable, nc.redundancy_eps,
              (q.redun_rows > 0) ? 100.0 * static_cast<double>(q.redun_uncheckable)
                                         / static_cast<double>(q.redun_rows)
                                 : 0.0,
              q.redun_uncheckable_hot, nc.redundancy_hot_sigma, q.redun_events,
              std::max(0, nc.redundancy_event_holdoff_ep));
    if (q.redun_uncheckable_hot > 0)
        log->warn("Network (redundancy census): WORST cross-flagged cell was {} at "
                  "s = {:.4e} — an UNCHECKABLE row carrying a {:.0f}-sigma centred "
                  "prefit deviation.  Its misfit is invisible to post-fit RAIM, to "
                  "sigma0 and to robust weighting by construction (MDB ~ 1/sqrt(r)), "
                  "so nothing downstream would have flagged it.  This is the parked-"
                  "garbage signature; read it against the topology at that epoch.",
                  q.redun_max_hot_id, q.redun_max_hot_s, q.redun_max_hot_s);
    // Prefer-fallback control.  Uncheckable rows that stay quiet are the
    // single-frequency risers the prefer-fallback directive keeps; naming the worst
    // of them makes "counted, and none was hot" a claim a reader can check.
    if (q.redun_uncheckable > q.redun_uncheckable_hot)
        log->info("Network (redundancy census): {} uncheckable row(s) stayed BELOW "
                  "the cross-flag and were counted, never evented and never "
                  "weight-touched (prefer-fallback directive).  Worst of them: {} at "
                  "s = {:.4e}.",
                  q.redun_uncheckable - q.redun_uncheckable_hot,
                  q.redun_max_cold_id.empty() ? std::string("(none finite)")
                                              : q.redun_max_cold_id,
                  q.redun_max_cold_s);
    log->info("Network (redundancy census INC-2): {} local column(s) examined, {} "
              "column-epoch(s) below the {:g} x median pivot floor; {} DECOUPLING "
              "edge(s) evented and {} RE-COUPLING edge(s) counted.",
              q.redun_pivot_cols, q.redun_pivot_flagged, nc.redundancy_pivot_eps,
              q.redun_unobs_entered, q.redun_unobs_cleared);
    // Closure: trace(H) over the swept rows can never exceed the number of free
    // parameters they span.  A negative slack means the covariance or the row stack
    // does not describe the system that was solved, which invalidates every r_i
    // above, so a violation is logged as a warning.
    if (q.redun_closure_any)
        log->info("Network (redundancy census): closure worst slack "
                  "n_free - trace(H) = {:.4e} over {} sweep(s) (must be >= 0); {} "
                  "row(s) hit the kernel's r in [0,1] clamp at 0, and {} centring "
                  "pool(s) were skipped for population < {}.",
                  q.redun_closure_min_slack, q.redun_sweeps, q.redun_r_clamped,
                  q.redun_pool_skips, kCensusMinPoolRows);
    if (q.redun_closure_neg > 0)
        log->warn("Network (redundancy census): {} sweep(s) VIOLATED the closure "
                  "bound.  Every redundancy number in this run is untrusted — do not "
                  "grade the cross-flag on it.", q.redun_closure_neg);
    // --- invariant #4: separate "ran and found nothing" from "never ran" ---
    if (q.redun_passes == 0)
        log->warn("Network (redundancy census): ARMED but the seam ran on ZERO "
                  "epochs — the wiring never executed.  Nothing in this run is "
                  "evidence about the card.");
    else if (q.redun_sweeps == 0)
        log->warn("Network (redundancy census): ARMED and the seam ran on {} "
                  "epoch(s), but NOT ONE sweep completed — with stride {} and "
                  "escalation {}, either no epoch matched the stride phase or every "
                  "rowRedundancy call threw.  The census is INERT despite looking "
                  "live.", q.redun_passes, nc.redundancy_census_stride,
                  (nc.redundancy_escalate_sigma > 0.0) ? "armed" : "off");
    else if (q.redun_rows == 0)
        log->warn("Network (redundancy census): ARMED, {} sweep(s) ran and ZERO DATA "
                  "rows were examined — every row classified as a pin or a coupling "
                  "row, or every station was skipped on a dimension mismatch.  The "
                  "statistic has no domain.", q.redun_sweeps);
    else if (q.redun_uncheckable == 0)
        log->info("Network (redundancy census): {} data row(s) swept and NOT ONE was "
                  "uncheckable at r < {:g}.  That is a genuine negative result — this "
                  "network's topology cross-checked everything it folded — and it is "
                  "the state in which the cross-flag CANNOT fire, so do not read a "
                  "quiet run as validation of the alarm.",
                  q.redun_rows, nc.redundancy_eps);
    // The escalation path needs both numbers to be readable.  "0 escalations" alone
    // is ambiguous, and under stride 1 it is guaranteed: the scheduled sweep already
    // covers every station, so no latch ever needs acting on.  Reporting only the
    // action would claim more than the counters support (the reverse of live
    // counters over dead wiring).
    if (nc.redundancy_escalate_sigma > 0.0) {
        if (q.redun_escalate_flagged == 0)
            log->info("Network (redundancy census): the free tier ran and NEVER "
                      "flagged a station — no station's centred prefit deviation "
                      "reached {:g} all run.  The targeted path had ZERO "
                      "opportunity, which is NOT evidence that it works.",
                      nc.redundancy_escalate_sigma);
        else if (q.redun_escalations == 0)
            log->info("Network (redundancy census): the free tier raised {} "
                      "station-epoch latch(es) over {:g} and NONE became a targeted "
                      "sweep — expected at stride {} , where the scheduled sweep "
                      "already covers every station.  Grade the targeted path only "
                      "on a stride-0 arm.",
                      q.redun_escalate_flagged, nc.redundancy_escalate_sigma,
                      nc.redundancy_census_stride);
        else
            log->info("Network (redundancy census): the free tier raised {} "
                      "station-epoch latch(es) over {:g}, of which {} became a "
                      "targeted single-station sweep the stride would have missed.",
                      q.redun_escalate_flagged, nc.redundancy_escalate_sigma,
                      q.redun_escalations);
    }
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
