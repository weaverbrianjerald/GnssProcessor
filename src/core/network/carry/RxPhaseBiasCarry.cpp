/**
 * @file RxPhaseBiasCarry.cpp
 * @brief The eq34 receiver phase bias carried inside the station's local block: the block
 *        columns (PASS B), the harvest with the border regression and the block row, the step
 *        test and the run-end report.
 *
 * The diagonal route (the time-link row and its harvest) stays where the chain datum builds it;
 * the header's table names both routes.
 */

#include "network/carry/RxPhaseBiasCarry.hpp"

#include "network/assembly/Responses.hpp"        // carryPastHorizon, prmRespond, ResponseSite
#include "network/assembly/StationBuild.hpp"     // StationBuild, StationCarryFrame
#include "network/carry/Coupling.hpp"            // LocalBlockCol
#include "network/carry/LocalCarryHarvest.hpp"   // HarvestBorder, StationHarvestBlock, networkHarvestColumn
#include "network/carry/SharedHistory.hpp"       // networkSharedHistoryNoteEntry
#include "network/driver/Epoch.hpp"              // NetworkEpoch
#include "network/driver/Run.hpp"                // NetworkRun
#include "network/driver/RunTypes.hpp"           // LocalCarry, PlHarvestBlock, StationAssembly
#include "network/runend/RunEnd.hpp"             // NetworkRunEndView
#include "network/solve/Estimator.hpp"           // NetworkEstimator::reconstructStation
#include "network/state/RunContext.hpp"          // the knobs, the station table, the counters
#include "network/state/StationParams.hpp"       // receiverPhaseBiasSignal

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

bool rxPbStepExceeds(double value_now, double var_now, double value_carried, double var_carried,
                     double q_m2s, double gap_s, double max_gap_s) {
    if (!(gap_s >= 0.0 && gap_s <= max_gap_s)) return false;   // the gap gate's entry, not a step
    const double var = var_now + var_carried + q_m2s * gap_s + kRxPbStepGuardVarM2;
    if (!(var > 0.0) || !std::isfinite(var)) return false;
    const double step = std::abs(value_now - value_carried);
    return std::isfinite(step) && step > kRxPbStepSigma * std::sqrt(var);
}

// ---- PASS B station steps ----

void networkRxPbBlockColumns(StationCarryFrame& carry) {
    // ---- From the station build ----
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    const double pl_now_s = build.clock.pl_now_s;
    const int i = build.i;
    StationAssembly& R = build.R;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const bool do_amb_carry = carry.do_amb_carry;
    const std::vector<PlPend>& pl_pend = carry.pl_pend;
    const std::vector<LocalBlockCol>& iono_bc = carry.iono_bc;
    const std::vector<LocalBlockCol>& rxclk_bc = carry.rxclk_bc;
    const std::vector<LocalBlockCol>& rxcb_bc = carry.rxcb_bc;
    std::vector<LocalBlockCol>& rpb_bc = carry.rpb_bc;
    const bool rpb_blk_on = ctx.knobs.rpb_blk_on;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;         // the bias's random-walk q (m^2/s)
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;     // the carry's gap horizon
    const net::StationTable& sta_rows = ctx.entities.stations;   // rpb: read-only in the parallel build
    // ---- End of the bindings ----

    if (!rpb_blk_on) return;
    if (!(legacy_local_inject && do_amb_carry)) return;
    // Whether another pending column of the station was harvested into the block `b`.  The
    // block pairs entries by block identity, so a receiver phase bias whose block no other
    // pending column shares would fold with every cross term zeroed: a diagonal prior at the
    // conditional variance, the double count this route removes.
    const bool others = !pl_pend.empty() || !iono_bc.empty() || !rxclk_bc.empty() || !rxcb_bc.empty();
    const auto shared = [&](const std::shared_ptr<const PlHarvestBlock>& b) {
        for (const PlPend& p : pl_pend)
            if (p.lc->blk == b) return true;
        for (const std::vector<LocalBlockCol>* cols : {&iono_bc, &rxclk_bc, &rxcb_bc})
            for (const LocalBlockCol& c : *cols)
                if (c.lc->blk == b) return true;
        return false;
    };
    // The pin step recorded every unpinned receiver phase-bias column with a live phase row
    // (pl_rpb: key, local column, linearization point); the datum receiver's pinned bias is
    // not among them.  The carry table is read-only here; the columns point at its entries.
    for (const auto& [rpb_key, rcol, rx_lin] : R.pl_rpb) {
        if (!std::isfinite(rx_lin)) continue;
        const LocalCarry* rc = net::get(sta_rows, StationId{i}, &net::StationState::signals, rpb_key,
                                        &net::StationSignalState::rpb);
        if (rc == nullptr) continue;   // cold: the harvest seeds it
        const double gap = pl_now_s - rc->last_seen_s;
        if (!(gap >= 0.0 && gap <= pl_max_gap_s)) {
            // The decoupling edge of a gap: past the horizon the entry does not fold.
            if (carryPastHorizon(pl_now_s, rc->last_seen_s, pl_max_gap_s)) ++R.prm_rpb_gap_skip;
            continue;
        }
        if (!rc->blk || rc->blk_idx < 0 || (others && !shared(rc->blk))) {
            ++R.pl_rxpb_no_block_skip;
            continue;
        }
        const double var_eff = rc->var + eq34_rpb_q * gap;
        if (!(var_eff > 0.0) || !std::isfinite(var_eff)) continue;
        LocalBlockCol col{rcol, rx_lin, rc, var_eff, gap, /*vce_fac=*/1.0};
        col.is_rx_pb = true;
        if (const auto sig = receiverPhaseBiasSignal(rpb_key)) col.signal = *sig;   // its block row's tag
        rpb_bc.push_back(col);
    }
}

// ---- End of the PASS B station steps ----

// ---- Harvest half ----

void networkHarvestRxPbBlock(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border,
                             const std::vector<StationHarvestBlock>& blocks) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const double pl_now_s = ep.clock.pl_now_s;
    const bool rpb_blk_on = ctx.knobs.rpb_blk_on;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    SharedHistoryHarvest& shx_h = border.shx_h;
    // ---- End of the bindings ----

    if (!rpb_blk_on) return;
    net::StationTable& sta_rows = ctx.entities.stations;   // the station carries
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (R.pl_rpb.empty()) continue;
        const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
        const StationId sid{static_cast<int>(i)};
        // The segment's block and first row: row0 + a is column a's row, read at every block
        // column of the station, so next epoch's block pairs it with them by block identity.
        const std::shared_ptr<PlHarvestBlock> blk = blocks[i].segmentBlock(BlockSegment::RxPhaseBias);
        const std::vector<Eigen::Index>* cross_cols = blk ? &blocks[i].cols : nullptr;
        const Eigen::Index row0 = blk ? blocks[i].rowOf(BlockSegment::RxPhaseBias) : Eigen::Index{-1};
        Eigen::VectorXd xr;
        try { xr = est.reconstructStation(est_idx, x_s); } catch (...) { continue; }
        for (std::size_t a = 0; a < R.pl_rpb.size(); ++a) {
            const auto& [rpb_key, rcol, rx_lin] = R.pl_rpb[a];
            if (rcol < 0 || rcol >= xr.size()) continue;
            // The posterior absolute and the pure conditional variance (N_rr^-1)_cc, with the
            // regression on the border and the cross terms to the station's block columns.
            const double value_abs = rx_lin + xr(rcol);
            if (!std::isfinite(value_abs)) continue;
            Eigen::VectorXd cross;
            auto hc = networkHarvestColumn(run, ep, border, est_idx, rcol, cross_cols,
                                           blk ? &cross : nullptr);
            const double var = hc.first;
            if (!(var > 0.0) || !std::isfinite(var)) {
                ++ctx.counters.n_rxpb_blk_harvest_fail;
                continue;
            }
            const LocalCarry* prev = net::get(sta_rows, sid, &net::StationState::signals, rpb_key,
                                              &net::StationSignalState::rpb);
            // Event response: a step between this harvest and the carried value.  The entry
            // is erased (reset, not repair) and not stashed again this epoch: this solve
            // folded the stale prior, so its posterior is suspect too, and the next harvest
            // seeds the entry from a solve the stale prior no longer reaches.
            if (prev != nullptr
                && rxPbStepExceeds(value_abs, var, prev->value_abs, prev->var, eq34_rpb_q,
                                   pl_now_s - prev->last_seen_s, pl_max_gap_s)) {
                net::drop(sta_rows, sid, &net::StationState::signals, rpb_key,
                          &net::StationSignalState::rpb);
                ++ctx.counters.n_rxpb_blk_step_reset;
                prmRespond(ctx.counters.prm_resp, ResponseSite::RpbStepErase);
                continue;
            }
            LocalCarry ent{value_abs, var, pl_now_s, std::move(hc.second)};
            ent.var_first = prev ? prev->var_first : var;
            localCarryStampBirth(ent, prev, static_cast<long long>(ctx.counters.n_loop_epochs),
                                 pl_now_s);
            bool in_blk = false;
            if (blk && row0 >= 0 && cross.size() == blk->C.rows()) {
                const Eigen::Index row = row0 + static_cast<Eigen::Index>(a);
                if (row < blk->C.rows()) {
                    blk->C.row(row) = cross.transpose();
                    ent.blk     = blk;
                    ent.blk_idx = static_cast<int>(row);
                    in_blk = true;
                }
            }
            if (!in_blk) ++ctx.counters.n_rxpb_blk_no_block;
            net::put(sta_rows, sid, &net::StationState::signals, rpb_key,
                     &net::StationSignalState::rpb) = std::move(ent);
            networkSharedHistoryNoteEntry(shx_h, est_idx, rcol, SharedHistoryMap::RxPb,
                                          static_cast<int>(i), rpb_key);   // shared history; no-op off
            ++ctx.counters.n_rxpb_blk_harvested;
        }
    }
}

// ---- End of the harvest half ----

// ---- Run-end reports ----

int networkRxPhaseBiasCarryRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (!ctx.knobs.rpb_blk) return 0;
    const auto& C = ctx.counters;
    log->info("Network (RPB-IN-BLOCK): {} receiver phase-bias column(s) admitted into station "
              "blocks, {} pushed but NOT folded (non-SPD block, no diagonal fallback), {} "
              "(receiver phase bias, other) pair(s) zeroed for a missing cross term, {} carried "
              "entr(y/ies) left out for want of a shared block row; harvest: {} stashed with a "
              "border regression, {} without a block row, {} coupling harvest(s) failed, {} "
              "erased by the step test (event response); {} entr(y/ies) carried at run end.",
              C.n_pl_blk_rxpb_cols, C.n_pl_blk_rxpb_not_folded, C.n_pl_blk_rxpb_missing_cross,
              C.n_rxpb_blk_no_block_skip, C.n_rxpb_blk_harvested, C.n_rxpb_blk_no_block,
              C.n_rxpb_blk_harvest_fail, C.n_rxpb_blk_step_reset,
              net::present(ctx.entities.stations, &net::StationState::signals,
                           &net::StationSignalState::rpb));
    if (!ctx.knobs.rpb_blk_on)
        log->warn("Network (RPB-IN-BLOCK): the key is set but the route is not live (it needs "
                  "the chain's carried receiver phase bias and a local block): the key is inert.");
    else if (C.n_pl_blk_rxpb_cols == 0)
        log->warn("Network (RPB-IN-BLOCK): ARMED but no receiver phase-bias column entered a "
                  "block over the whole run: the key is inert.");
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
