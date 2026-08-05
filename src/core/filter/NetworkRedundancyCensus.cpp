/**
 * @file NetworkRedundancyCensus.cpp
 * @brief Redundancy / testability census — see NetworkRedundancyCensus.hpp for
 *        the design argument (docs/SPEC_redundancy_census.md).
 *
 * Measure-only.  Nothing here writes a weight, a state or a decision; the whole
 * translation unit reads the SOLVED system and emits counters + events.
 *
 * ## The closure check (standing invariant #4, statistics edition)
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
 * survives restriction to ANY subset of rows — which is what makes it usable on
 * a targeted single-station sweep.  The census therefore reports the SLACK
 * @f$n_\text{free} - \sum h_{ii}@f$ and treats a negative slack as a defect in
 * the covariance/leverage path, not as a finding about the data.
 *
 * Two honest caveats, both recorded rather than papered over:
 *  - the kernel CLAMPS @f$r_i@f$ to [0,1], so a genuinely over-unity leverage
 *    is reported as @f$h = 1@f$; the clamp count is a separate counter, and the
 *    slack is consequently a conservative (never false-alarming) statistic;
 *  - @f$n_\text{free}@f$ counts the shared columns that are NOT exact gauges —
 *    measured directly as the non-zero columns of the datum-frame covariance,
 *    so both the datum pins and the ensemble phase gauges are excluded without
 *    this file having to know which mechanism zeroed them.
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
#include "filter/NetworkBlockBaarda.hpp" // BlockFormal LOS-block rider on this sweep (SPEC_block_baarda)
#include "filter/NetworkCongruence.hpp" // Station congruence rider on this sweep (SPEC_station_congruence)
#include "filter/NetworkMdb.hpp"        // Baarda MDB rider on the swept r_i (SPEC_network_mdb_reliability)
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"

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
    // receiver clock, so it is standardized RAW and joins no pool.
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
    // Median over the FINITE POSITIVE pivots: a non-positive pivot is already a
    // finding and must not be allowed to drag the reference scale down with it.
    std::vector<double> good;
    good.reserve(static_cast<std::size_t>(n));
    for (Eigen::Index k = 0; k < n; ++k)
        if (std::isfinite(pivots(k)) && pivots(k) > 0.0) good.push_back(pivots(k));
    if (good.empty()) {                    // every column degenerate — flag them all
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
 * its state index landed on.  A column no map claims keeps the positional
 * fallback @c "loc<col>" — a census that cannot name a column must still count
 * it, and an unnamed finding is far better than a dropped one.
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

void networkRedundancyCensus(NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             NetworkRunTraces& traces, const SyncedEpoch& se,
                             const std::vector<NetStation>& stations,
                             const NetworkEstimator& est,
                             const NetworkEstimator::Datum& plan_datum,
                             const std::vector<StationAssembly>& asm_res,
                             const std::vector<int>& est_index_of,
                             const Eigen::VectorXd& x_s) {
    // Alias preamble (seam contract) — the body reads the driver's names.
    const auto& nc      = config_.getNetworkConfig();
    auto& carry         = ctx.carry;
    auto& counters      = ctx.counters;
    const double eps        = nc.redundancy_eps;
    const double hot_sigma  = nc.redundancy_hot_sigma;
    const double pivot_eps  = nc.redundancy_pivot_eps;
    const double escal_sig  = nc.redundancy_escalate_sigma;
    const int    stride     = nc.redundancy_census_stride;
    const int    holdoff    = std::max(0, nc.redundancy_event_holdoff_ep);
    // docs/SPEC_network_mdb_reliability.md INC-0: the MDB rider on this sweep's
    // r_i.  Swept-row MDB inherits the census's stride and escalation exactly —
    // no new sweeps, no second covariance product, one division per swept row.
    const bool   mdb_on     = ctx.knobs.mdb_on && ctx.knobs.mdb_from_census;
    const double mdb_delta0 = baardaDelta0(config_.dataSnoopingCritical());
    // docs/SPEC_block_baarda.md INC-1: the BlockFormal rider on this sweep's
    // SWEPT stations.  It inherits the stride, the targeted escalation and the
    // trip-forced full sweep exactly — SPEC §4 makes "no new scheduler" the
    // design constraint, which is why it lives here rather than in a seam of its
    // own.  It needs the POST-FIT residual, so an empty x_s disarms it: a rider
    // that silently censused the PREFIT innovation would produce a plausible
    // ladder of the wrong quantity.
    const bool bb_on = ctx.knobs.bb_on && ctx.carry.bb.enabled() && x_s.size() > 0;
    // docs/SPEC_station_congruence.md INC-1: the SECOND rider on this sweep — the
    // coordinate-fault screen in position space.  Same host, same stride, same
    // post-fit residual (formed once below and handed to both riders), and the
    // same empty-x_s disarm: a screen run on the PREFIT innovation would produce a
    // plausible delta_hat of the wrong quantity.
    const bool cg_on = ctx.knobs.cg_on && ctx.carry.cg.enabled() && x_s.size() > 0;

    const auto t0 = std::chrono::steady_clock::now();
    ++counters.redun_passes;
    const long long ep = carry.redun_epochs++;   // stride phase; 0 on the first pass

    // ---- the epoch's scalars are BLANK unless this pass fills them ---------
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

    // ---- who gets the COSTLY sweep this epoch ------------------------------
    // A flight-recorder epoch always forces a FULL sweep, so the attribution dump
    // and the census describe the SAME epoch — that pairing is the whole value of
    // "the next discharge names its own injector in the run where it happens".
    // Two channels, both read-only HERE (each dump seam runs later in this same
    // SB_FRAMEDIAG block and owns its own consumption):
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
        // Targeted: the stations the FREE tier flagged last epoch.
        for (std::size_t i : carry.redun_escalate_pending)
            if (i < asm_res.size() && i < est_index_of.size() && est_index_of[i] >= 0)
                sweep_set.insert(i);
        if (!sweep_set.empty()) counters.redun_escalations +=
            static_cast<long long>(sweep_set.size());
    }
    carry.redun_escalate_pending.clear();

    // The FREE tier walks every station when escalation is armed (that is the
    // whole point — it decides who gets swept NEXT epoch); otherwise it walks
    // only the stations being swept, because that is exactly where the
    // cross-flag needs the magnitude half.
    std::set<std::size_t> census_set = sweep_set;
    if (escal_sig > 0.0)
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i)
            if (i < est_index_of.size() && est_index_of[i] >= 0) census_set.insert(i);
    if (census_set.empty()) return;

    const SharedColumnMap cm = buildSharedColumnMap(est, n_shared);

    // Datum-frame shared covariance, built ONCE (the per-station wrapper would
    // copy the whole n_shared² block per call — 63 × 450² doubles on the FT63
    // tier).  Exact gauges are zeroed inside; n_free counts what is left.
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

        // ---- FREE tier: classes + the centred magnitude metric -------------
        const Eigen::Index m = R.y.size();
        std::vector<NetRowClass> cls(static_cast<std::size_t>(m));
        std::vector<std::string> sat(static_cast<std::size_t>(m));
        for (Eigen::Index row = 0; row < m; ++row)
            cls[static_cast<std::size_t>(row)] =
                classifyRow(cm, R, row, &sat[static_cast<std::size_t>(row)]);
        const StationPrefitCensus pc = stationPrefitCensus(cls, R);
        counters.redun_pool_skips += pc.n_pool_skipped;

        // Escalation latch: this station's largest centred deviation decides
        // whether it gets a TARGETED sweep next epoch.  Latched even when the
        // station is being swept right now — a park persists, and the next
        // epoch's verdict is the one that confirms it.
        if (escal_sig > 0.0 && pc.max_s > escal_sig) {
            carry.redun_escalate_pending.insert(i);
            ++counters.redun_escalate_flagged;   // OPPORTUNITY, not action
        }

        if (sweep_set.find(i) == sweep_set.end()) continue;   // free tier only

        // ---- COSTLY tier: per-row redundancy ------------------------------
        Eigen::VectorXd r;
        try { r = kernel.rowRedundancy(slot, R.A_s, R.A_r, R.w, cov_free); }
        catch (...) { continue; }
        if (r.size() != m) continue;
        swept_any = true;
        ++counters.redun_station_sweeps;
        n_local_swept += kernel.numLocal(slot);

        // Closure domain is EVERY row (pins and coupling rows carry leverage
        // too); the census statistics below are DATA rows only.
        for (Eigen::Index row = 0; row < m; ++row) {
            const double h = 1.0 - r(row);
            if (std::isfinite(h)) trace_h += h;
            if (r(row) <= 0.0) ++ep_clamped;
        }
        counters.redun_rows_all += static_cast<long long>(m);

        // The MDB rider's feed for this station.  Constructed INSIDE the swept
        // branch so it sees exactly the rows the census statistics below see —
        // that identity of domain is what the run-end uncheckable JOIN checks.
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
            // One division per swept DATA row.  Fed unconditionally (before every
            // usability screen below) so this seam's independent recount of
            // "data rows" and "rows below eps" is over the SAME set the census
            // counts — the join is only a check if both walk one domain.
            if (mdb_on)
                mdb_feed->add(sat[static_cast<std::size_t>(row)], k,
                              (row < R.w.size()) ? R.w(row) : 1.0, r(row));
            const double s = pc.s[static_cast<std::size_t>(row)];
            if (!(r(row) < eps)) continue;
            ++sta_unchk;                     // uncheckable: counted, never acted on
            if (!std::isfinite(s)) continue;
            if (s > ep_unchk_max_s) ep_unchk_max_s = s;
            // ---- THE CROSS-FLAG ------------------------------------------
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
                // Rate limit per (station, satellite, class): a parked cell must
                // log its identity persistently, not 120x/hour.
                const std::string key = sname + "\x1f"
                                      + sat[static_cast<std::size_t>(row)] + "\x1f"
                                      + rowClassName(k);
                const auto lit = carry.redun_event_last.find(key);
                if (lit == carry.redun_event_last.end() || (ep - lit->second) >= holdoff) {
                    carry.redun_event_last[key] = ep;
                    emit("uncheckable_hot_cell", sat[static_cast<std::size_t>(row)],
                         sname, s, hot_sigma,
                         fmt::format("class={};r={:.3e};eps={:g};y={:.4e};w={:.4e};row={}",
                                     rowClassName(k), r(row), eps, R.y(row),
                                     (row < R.w.size()) ? R.w(row) : 1.0, row));
                }
            } else if (s > counters.redun_max_cold_s) {
                // The fallback-directive control: an uncheckable row that is NOT
                // hot is counted and NEVER evented (a healthy single-frequency
                // riser lives here).  Keeping the worst one makes "we counted
                // them and none was hot" a checkable claim at run end.
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

        // ---- BLOCKFORMAL rider: the (station, satellite) LOS blocks ---------
        // docs/SPEC_block_baarda.md INC-1.  Same station, same rows, same
        // hoisted covariance, same kernel slot the row census just used — the
        // whole cost delta is one back-substitution, two GEMVs and a b×b solve
        // per block with b ≤ 8.
        if (bb_on || cg_on) {
            // The post-fit residual.  The census's own statistic needs none of
            // this (r_i is a property of the geometry), but T_B does, and
            // StationAssembly::y is the PREFIT innovation.  The estimator wrapper
            // is used deliberately over the raw kernel method so this cost lands
            // in the D0 cov_consumers_ms line and SPEC §5's budget stays checkable.
            //
            // BOTH riders read it, and it is formed ONCE: the congruence screen
            // (docs/SPEC_station_congruence.md) needs the same y − A_s x_s − A_r x_r
            // over the same station, so a second reconstructStation here would
            // double the one genuinely expensive call in either card's budget.
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(slot, x_s); }
            catch (...) { x_r.resize(0); }
            if (x_r.size() != R.A_r.cols()) {
                if (bb_on) ++ctx.counters.bb.no_resid;
                if (cg_on) ++ctx.counters.cg.no_resid;
            } else {
                const Eigen::VectorXd resid = R.y - R.A_s * x_s - R.A_r * x_r;
                // The RESPONDED label's source, built once per station.  This is
                // an ESTIMATOR-side signal (RAIM / slip refloat already marked the
                // arc for reseeding this epoch) and is deliberately NOT the
                // slip-QC raw verdict that INC-2 corroborates against: if the
                // label and the corroboration shared a source, SPEC §8.2's
                // positive control — the RESPONDED ladder against the CLEAN one —
                // would be circular and could not fail.
                //
                // The SEAM ORDER is what makes this readable at all, and it is
                // load-bearing: SB_LEVELING's carry harvest CLEARS the pending-
                // reseed set (NetworkRunContext.cpp, clearPendingReseed) and only
                // THEN does SB_RAIM mark this epoch's refloats — so by
                // SB_FRAMEDIAG the set holds exactly the arcs an estimator-side
                // mechanism responded to THIS epoch, which is the population the
                // label means.  Move this feed above SB_RAIM and every block would
                // silently read CLEAN.
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

                // ---- CONGRUENCE rider: the station's coordinates -------------
                // docs/SPEC_station_congruence.md INC-1.  AFTER the block sweep,
                // deliberately: INC-3's separability join needs to know whether
                // the per-LOS tier flagged anything at THIS station THIS epoch,
                // and the hit-count delta is that verdict at its source.  Reading
                // it from ctx.carry.bb_hits later would couple this card to the
                // block census's emission seam, which DRAINS that vector.
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

        // ---- INC-2: unobservable local columns ----------------------------
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
            // Both edges (proposed invariant #7): the DECOUPLING edge is an event
            // with an identity, the RE-COUPLING edge is counted.  Rebuilding the
            // per-station set from scratch is what makes a column that recovers
            // re-eventable if it goes unobservable again.
            std::set<std::string>& prev = carry.redun_unobs_active[i];
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

        // The L1 epoch-status scalars, valid for THIS epoch only (the writer
        // compares the stamp, so a non-sweep epoch leaves the columns blank
        // rather than repeating a stale sweep's numbers).
        carry.redun_ep_tow          = se.time.tow;
        carry.ep_redun_rows         = ep_rows_data;
        carry.ep_uncheckable_rows   = ep_unchk;
        carry.ep_uncheckable_max_s  = ep_unchk_max_s;
        carry.ep_unobservable_local = ep_unobs;
        // The per-epoch WORST IDENTITY is deliberately not carried: a hot cell
        // reaches network_events.csv with its full identity and the run worst
        // lives in the counters, so a third copy would be state nothing reads.
    }

    counters.redun_wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

}  // namespace GnssProcessor
