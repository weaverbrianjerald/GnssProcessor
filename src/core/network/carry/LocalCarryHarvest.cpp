/**
 * @file LocalCarryHarvest.cpp
 * @brief The station-local carries' post-solve harvest: the orchestrator and the machinery its
 *        kinds share (the border, the column regression, the station layout).
 *
 * The orchestrator below calls each kind's harvest in the file of its carry (the table of
 * contents is in network/carry/LocalCarryHarvest.hpp).
 */

#include "network/carry/LocalCarryHarvest.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/Epoch.hpp"
#include "network/state/RunContext.hpp"   // networkResumeFirstEpochRetire (network/state/WarmResume.cpp)
#include "network/diag/AmbStateTrace.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/carry/LevelingCarry.hpp"
#include "network/carry/PhaseColourCarry.hpp"
#include "network/carry/RxClockCarry.hpp"
#include "network/carry/RxCodeBiasCarry.hpp"
#include "network/carry/RxPhaseBiasCarry.hpp"
#include "network/carry/SlantIonoCarry.hpp"
#include "network/carry/ZwdCarry.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, traitsOf, SignalId, nominalCarrierFrequencyHz
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan
#include "filter/StateKeySchema.hpp"         // StateKeys: ambiguity and phase-bias keys, GLONASS IFB keys

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {

double networkEpochSpacingS(double now_s, double prev_s, double max_gap_s) {
    double dt_ep = now_s - prev_s;
    if (!std::isfinite(dt_ep) || dt_ep <= 0.0 || dt_ep > max_gap_s)
        dt_ep = 0.0;   // first epoch / implausible spacing ⇒ gap_ep = 1
    return dt_ep;
}

void networkHarvestLocalCarries(NetworkRun& run, const NetworkEpoch& ep, EpochEvents& events) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    std::vector<NetStation>& stations = run.setup.stations;
    bool carry_warm = ep.clock.carry_warm;
    double pl_now_s = ep.clock.pl_now_s;
    // ---- End of the bindings ----
    networkHarvestHandoverLedger(run, ep);

    // --- Phase-leveling carry: harvest posterior and stash ----
    // For each present station (the pivot too under the prioritized datum), recover its
    // ambiguities' absolute values (x_lin + back-substituted Δx) and their conditional
    // variances (the cheap per-column diagonal harvest, not the full per-station inverse),
    // and refresh the persistent carry.  Then drop entries unseen beyond the max gap (a
    // stale-carry and map-growth guard, distinct from the slip-driven refloat/drop).
    if (carry_warm) {
        HarvestBorder border = networkHarvestBorderBegin(run, ep);
        std::vector<StationHarvestBlock> blocks(stations.size());
        // network.parallel_carry_harvest: the layouts first (they read only the solution and the
        // assembly), then every carried column's regression into its own slot, in parallel; the
        // walk below reads the slots through networkHarvestColumn and keeps every write serial.
        // A clone regression is not taken ahead: its row step writes the border.
        const bool ahead = networkHarvestParallel(run) && ctx.knobs.carry_cpl_cond
                           && !border.ccl_h.on && !border.ccl_h.noreg;
        if (ahead) {
            for (size_t i = 0; i < stations.size(); ++i)
                blocks[i] = networkHarvestStationBlock(run, ep, border, i);
            networkHarvestColumnsAhead(run, ep, blocks, border);
        }
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!ahead) blocks[i] = networkHarvestStationBlock(run, ep, border, i);
            if (!blocks[i].ok) continue;
            networkHarvestAmbiguityStation(run, ep, border, blocks[i], i);
            networkAmbIfTraceStation(run, ep, blocks[i], i, events);
            networkHarvestPhaseColourStation(run, ep, border, blocks[i], i, events);
        }
        // Stamp this harvest: the next epoch differences it against its own stamp for
        // the observed inter-epoch spacing.
        ctx.carry.pl_prev_now_s = pl_now_s;
        networkPrunePhaseColour(run, ep);
        networkPruneAmbiguities(run, ep, events);
        networkHarvestSlantIonoBlock(run, ep, border, blocks);
        networkHarvestRxClock(run, ep, border, blocks);
        networkHarvestRxCbBlock(run, ep, border, blocks);
        networkHarvestRxPbBlock(run, ep, border, blocks);
        networkHarvestZwd(run, ep, border, blocks);
        networkHarvestBorderEnd(run, ep, border);
        networkHarvestRxPhaseBias(run, ep);
    }
    networkRxCbDatumGaugeCensus(run, ep);
    networkHarvestRxCbCarry(run, ep);
    networkResumeFirstEpochRetire(run);
}

HarvestBorder networkHarvestBorderBegin(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const NetworkEstimator& est = run.engines.est;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    const SphericalHarmonicIonosphere* sh_model = run.setup.sh_model.get();
    const SphericalHarmonicIonosphere* sh_model2 = run.setup.sh_model2.get();
    const std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys = ep.columns.registered_code_keys;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    // ---- End of the bindings ----
    HarvestBorder border;
    // Build, once per epoch, the shared column → identity descriptor map and the
    // datum-frame marginal-variance diagonal, so each carried local's regression on
    // the free shared border can be stashed in a layout-stable (identity-keyed) form.
    // Empty and unused on the legacy diagonal path → byte-identical.
    std::map<Eigen::Index, CoupledShared>& col2desc = border.col2desc;
    Eigen::MatrixXd& cov_df = border.cov_df;   // shared covariance, datum-pinned + ensemble-gauge cols zeroed
    // The clones' posterior regression on the rest of the border, once per
    // epoch (network/carry/ClockClone.hpp).  Inert off and when no clone is registered.
    ClockCloneHarvest& ccl_h = border.ccl_h;
    if (carry_cpl_cond) {
        auto regcol = [&](CoupledShared d, Eigen::Index col) {
            if (col >= 0 && col < est.numShared()) col2desc.emplace(col, std::move(d));
        };
        for (const std::string& sat : registered_clock_sats) {
            try { CoupledShared d; d.kind = SharedParamKind::Clock; d.sat = sat;
                  regcol(std::move(d), est.clockColumn(sat)); } catch (...) {}
        }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (!parts) continue;
            try { CoupledShared d; d.kind = SharedParamKind::PhaseBias;
                  d.sat = parts->satellite; d.signal = parts->signal;
                  regcol(std::move(d),
                         est.phaseBiasColumn(parts->satellite, parts->signal)); } catch (...) {}
        }
        for (const auto& kv : registered_code_keys) {
            try { CoupledShared d; d.kind = SharedParamKind::CodeBias;
                  d.sat = kv.first; d.signal = kv.second;
                  regcol(std::move(d), est.codeBiasColumn(kv.first, kv.second)); } catch (...) {}
        }
        if (config_.estimateNetworkIonoSh() && sh_model)
            for (const ShLayer& lay :
                 shLayers(config_.getGlobalVtecShConfig(), sh_model, sh_model2))
                for (const auto& c : lay.model->coeffs()) {
                    try { CoupledShared d; d.kind = SharedParamKind::VtecSh;
                          d.n = c.n; d.m = c.m; d.trig = c.trig;
                          d.sh_layer = lay.layer;
                          regcol(std::move(d),
                                 est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer)); } catch (...) {}
                }
        // P_prev,jj, with which a departed shared column is marginalized next epoch:
        // the datum-frame marginal variance (datum-pinned and ensemble-gauge columns
        // exactly 0, mirroring conditionalLocalVarianceDiag's frame).
        cov_df = est.sharedCovariance();
        for (const auto& p : plan.shared_datum)
            if (p.column >= 0 && p.column < cov_df.rows()) {
                cov_df.row(p.column).setZero(); cov_df.col(p.column).setZero();
            }
        est.zeroPhaseGaugeColumns(cov_df);
        ccl_h = networkClockCloneHarvestBegin(ctx, est, plan.shared_datum, col2desc);
        border.desc_of_col.assign(static_cast<std::size_t>(est.numShared()), nullptr);
        for (const auto& [col, desc] : col2desc)
            border.desc_of_col[static_cast<std::size_t>(col)] = &desc;
    }
    // Where each harvested row's entry is written, for the compression at
    // the end (network/carry/SharedHistory.hpp).  Inert off.
    border.shx_h = networkSharedHistoryHarvestBegin(ctx, ccl_h);
    return border;
}

// Harvest one carried column's pure conditional variance (N_rr⁻¹)_cc and its
// regression on the free shared border (identity-keyed, sparsified).
// Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
// With the harvest block (network.phase_leveling_local_block or the float_kf vehicle),
// non-null `cross_cols` / `cross` make the same per-column solve also read the
// station's other carried columns and return them as (N_rr⁻¹)_{c c_j} (a gather, no
// second solve); both are null on every other call (byte-identical).
std::pair<double, std::vector<CoupledShared>> networkHarvestColumn(
    NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border, std::size_t est_idx,
    Eigen::Index col, const std::vector<Eigen::Index>* cross_cols, Eigen::VectorXd* cross) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    ClockCloneHarvest& ccl_h = border.ccl_h;
    // ---- End of the bindings ----
    // Computed ahead of this walk (network.parallel_carry_harvest): the slot is read once.
    if (est_idx < border.ahead.size()
        && col >= 0 && col < static_cast<Eigen::Index>(border.ahead[est_idx].size())) {
        HarvestColumnAhead& a = border.ahead[est_idx][static_cast<std::size_t>(col)];
        if (a.ready && a.with_cross == (cross_cols != nullptr)) {
            a.ready = false;
            // One consumer call in the section timers' count; the region's wall stays in the
            // block's net (a sum of thread times would exceed the block's wall).
            if (est.normals().perfTiming()) est.normals().accumulateCovConsumer(0.0);
            ++ctx.counters.n_harvest_ahead_read;
            if (cross) *cross = std::move(a.cross);
            return {a.cond_var, std::move(a.coupling)};
        }
    }
    NetworkNormalEquations::StationRegression reg;
    try { reg = est.stationRegression(est_idx, col, /*carry_aware=*/false, cross_cols); }
    catch (...) { return {-1.0, {}}; }
    if (cross) *cross = std::move(reg.cross);
    // The row's coefficients on the clock clones regressed onto the rest of
    // the border and the clones' conditional variance added (the clone columns are
    // not in col2desc, so without this the walk below would drop them).  No-op off.
    networkClockCloneHarvestRow(ccl_h, ctx, est_idx, col, reg.row, reg.cond_var);
    return {reg.cond_var, networkHarvestCouplingRow(reg.row, border, x_s)};
}

std::vector<CoupledShared> networkHarvestCouplingRow(const Eigen::VectorXd& row,
                                                     const HarvestBorder& border,
                                                     const Eigen::VectorXd& x_s) {
    const std::vector<const CoupledShared*>& desc_of_col = border.desc_of_col;
    const Eigen::MatrixXd& cov_df = border.cov_df;
    std::vector<CoupledShared> cpl;
    const Eigen::Index ns = row.size();
    if (ns > 0) {
        const double maxabs = row.cwiseAbs().maxCoeff();
        const double thr = kCouplingSparsifyRelThresh * maxabs;
        const auto described = [&](Eigen::Index j) {
            return j < static_cast<Eigen::Index>(desc_of_col.size())
                   && desc_of_col[static_cast<std::size_t>(j)] != nullptr;
        };
        std::size_t n_kept = 0;
        for (Eigen::Index j = 0; j < ns; ++j)
            if (std::abs(row(j)) > thr && described(j)) ++n_kept;
        cpl.reserve(n_kept);
        for (Eigen::Index j = 0; j < ns; ++j) {
            const double mj = row(j);
            if (std::abs(mj) <= thr) continue;          // sparsify near-zero entries
            if (!described(j)) continue;                // col not in the enumerated shared set
            // A datum-pinned or ensemble-gauge column is kept in the row
            // (coefficient and its ŝ_prev RHS term) with p_prev = 0 ("exactly
            // known"; the zeroed cov_df diagonal mirrors
            // conditionalLocalVarianceDiag's frame).  The full regression row is
            // exactly gauge-invariant: every station data row satisfies
            // A_s·g_s + A_r·g_r = 0 for each structural gauge direction, so
            // M = e_cᵀN_rr⁻¹N_rs has Mᵀg_s = −g_r(c), and the row's +1 local
            // completes the cancellation.  Dropping the pinned columns'
            // coefficients would leave M'ᵀg ≠ 0: every coupling row would then
            // observe the clock gauge, and the null-space rank gate would find fewer
            // near-null directions than declared and withhold every epoch that
            // carries rows.  Under the datum the pinned term is exact
            // (Dirichlet elimination), and p_prev = 0 degrades every other path
            // correctly: the κ time update falls through unshrunk (a pinned column
            // does not move), and a departed pinned column marginalizes to +0.
            const double pj = (j < cov_df.rows() && cov_df(j, j) > 0.0)
                                  ? cov_df(j, j) : 0.0;
            cpl.push_back(*desc_of_col[static_cast<std::size_t>(j)]);
            CoupledShared& cs = cpl.back();
            cs.coeff  = mj;
            cs.s_prev = x_s(j);
            cs.p_prev = pj;
        }
    }
    return cpl;
}

bool networkHarvestParallel(const NetworkRun& run) {
    return run.config.getNetworkConfig().parallel_carry_harvest && run.setup.parallel_on
           && run.setup.n_threads > 1 && Eigen::nbThreads() == 1;
}

void networkHarvestStationColumnsAhead(const NetworkNormalEquations& kernel,
                                       const HarvestBorder& border, const Eigen::VectorXd& x_s,
                                       std::size_t est_idx, const StationHarvestBlock& sb,
                                       std::vector<HarvestColumnAhead>& slots) noexcept {
    try {
        // The walk asks for the block's columns with the block's cross terms, or for none.
        const std::vector<Eigen::Index>* cross_cols = sb.blk ? &sb.cols : nullptr;
        for (const Eigen::Index col : sb.cols) {
            if (col < 0 || col >= static_cast<Eigen::Index>(slots.size())) continue;
            HarvestColumnAhead& a = slots[static_cast<std::size_t>(col)];
            if (a.ready) continue;
            NetworkNormalEquations::StationRegression reg =
                kernel.stationRegression(est_idx, col, /*carry_aware=*/false, cross_cols);
            a.coupling   = networkHarvestCouplingRow(reg.row, border, x_s);
            a.cross      = std::move(reg.cross);
            a.cond_var   = reg.cond_var;
            a.with_cross = (cross_cols != nullptr);
            a.ready      = true;
        }
    } catch (...) {
        // An allocation failure or a kernel refusal: the walk computes what is missing.
    }
}

void networkHarvestColumnsAhead(NetworkRun& run, const NetworkEpoch& ep,
                                const std::vector<StationHarvestBlock>& blocks,
                                HarvestBorder& border) {
    // ---- From the run and the epoch ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkNormalEquations& kernel = run.engines.est.normals();
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const int n_threads = run.setup.n_threads;
    // ---- End of the bindings ----
    std::vector<std::size_t> sta;   // the harvested stations, in station order
    std::size_t n_slots = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (!blocks[i].ok) continue;
        sta.push_back(i);
        n_slots = std::max(n_slots, static_cast<std::size_t>(est_index_of[i]) + 1);
    }
    border.ahead.assign(n_slots, {});
    for (const std::size_t i : sta) {
        border.ahead[static_cast<std::size_t>(est_index_of[i])].resize(
            static_cast<std::size_t>(blocks[i].xr.size()));
        ctx.counters.n_harvest_ahead_cols += static_cast<long long>(blocks[i].cols.size());
    }
    const HarvestBorder& border_read = border;
    std::vector<std::vector<HarvestColumnAhead>>& slots = border.ahead;
    #pragma omp parallel for schedule(dynamic) num_threads(n_threads)
    for (long long k = 0; k < static_cast<long long>(sta.size()); ++k) {
        const std::size_t i = sta[static_cast<std::size_t>(k)];
        const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
        networkHarvestStationColumnsAhead(kernel, border_read, x_s, est_idx, blocks[i], slots[est_idx]);
    }
}

StationHarvestBlock networkHarvestStationBlock(NetworkRun& run, const NetworkEpoch& ep,
                                               HarvestBorder& border, std::size_t i) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    double pl_now_s = ep.clock.pl_now_s;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   pl_local_block = ctx.knobs.pl_local_block; // stash the harvest cross-covariance block beside the per-column carry
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // ZWD in the local block (network.carry_zwd_in_block): the ZWD column joins the
    // harvest block.  It is gated on the same live predicate as the block itself
    // (pl_local_block && carry_cpl_cond), so the harvest and the inject agree on
    // which station-epochs have a block.  Off ⇒ `cols` is untouched and
    // PlHarvestBlock::C keeps its dimension without the ZWD column (byte-identical).
    const bool   zwd_blk_on  = ctx.knobs.zwd_blk_on;
    // Iono in the local block (network.carry_iono_in_block): the per-LOS iono columns
    // join the same harvest block, on the same live predicate, for the same reason.
    // Off ⇒ `cols` is untouched and PlHarvestBlock::C keeps its dimension
    // (byte-identical).
    const bool   iono_blk_on = ctx.knobs.iono_blk_on;
    // Receiver clock in the local block (network.carry_rx_clock_in_block): the
    // receiver-clock (and ISB) columns join the same harvest block, on the same live
    // predicate, for the same reason.  Off ⇒ `cols` is untouched and PlHarvestBlock::C
    // keeps its dimension (byte-identical).
    const bool   rxclk_blk_on  = ctx.knobs.rxclk_blk_on;
    // Receiver code bias in the local block (network.carry_rx_code_bias_in_block): the
    // receiver code-bias columns join the same harvest block and are stashed with their
    // border regression and block row here; the standalone diagonal harvest then
    // skips its stash.  Off => `cols` is untouched (byte-identical).
    const bool   rxcb_blk_on   = ctx.knobs.rxcb_blk_on;
    // The eq34 receiver phase bias in the local block (network.phase_datum_eq34_rpb_in_block):
    // the unpinned receiver phase-bias columns join the same harvest block.  Off => `cols` is
    // untouched (byte-identical).
    const bool   rpb_blk_on    = ctx.knobs.rpb_blk_on;
    ClockCloneHarvest& ccl_h = border.ccl_h;
    // ---- End of the bindings ----
    StationHarvestBlock sb;
    if (est_index_of[i] < 0) return sb;
    // Prioritized datum: harvest the former pivot too.  Its non-anchored arcs are
    // in R.pl_amb, and anchored (pinned) keys were excluded at build, so inject
    // and harvest partition on the same per-(sat,signal) predicate.
    if (!phase_datum_prioritized && stations[i].is_pivot) return sb;  // legacy: pivot gauge-pinned, never carried
    const StationAssembly& R = asm_res[i];
    // Iono in the local block: a station with no carry-eligible ambiguities can
    // still have live per-LOS iono columns, and PASS B builds a block for that
    // case (its block gate admits a live iono carry).  Skipping it
    // here would stash those entries without a block, so every pair with them
    // would be zeroed next epoch, giving a diagonal iono prior (the vehicle the
    // in-block carry replaces).  Harvest and inject have to agree on which
    // station-epochs have a block.  Off ⇒ the extra terms are false and this reduces
    // to `if (R.pl_amb.empty()) return sb;` (byte-identical).
    // Receiver clock in the local block: the same holds for a station whose only
    // live carry is its receiver clock.  PASS B builds a block for that case too,
    // and skipping it here would stash the entry without a block, diagonalizing
    // the most station-common direction of all.
    if (R.pl_amb.empty() && !(iono_blk_on && !R.pl_iono.empty())
        && !(rxclk_blk_on && !R.pl_rxclk.empty())
        && !(rxcb_blk_on && !R.pl_cb.empty())
        && !(rpb_blk_on && !R.pl_rpb.empty())
        && !(ctx.knobs.pc_on && !R.pl_pc.empty())) return sb;
    const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
    std::vector<Eigen::Index> cols;
    cols.reserve(R.pl_amb.size() + 1 + R.pl_iono.size() + R.pl_rxclk.size());
    for (const auto& a : R.pl_amb) cols.push_back(a.col);
    // Iono in the local block: the station's present per-LOS iono columns become
    // block indices n_amb … n_amb+k−1, appended before the ZWD so the ZWD keeps
    // its invariant that the appended column is the last row.  Every per-column
    // solve below then gathers (N_rr⁻¹)_{c_a, icol} at no extra cost, the same
    // gather that already covers the ambiguity-ambiguity terms.  R.pl_iono is
    // PASS B's record of what it pushed, so a station whose block was never built
    // keeps `cols` as it was.
    const Eigen::Index iono_row0 = static_cast<Eigen::Index>(cols.size());
    const bool iono_in_cols = iono_blk_on && !R.pl_iono.empty();
    if (iono_in_cols)
        for (const auto& t : R.pl_iono) cols.push_back(std::get<1>(t));
    // ZWD in the local block: the station's ZWD column becomes the block's last index,
    // so every per-column solve below gathers its
    // (N_rr⁻¹)_{c_a, zcol} cross term at no extra cost, the same gather that
    // already covers the ambiguity-ambiguity terms.  It is appended after the
    // ambiguity columns so the pl rows keep their indices `a`; networkHarvestZwd
    // writes the ZWD's own row.  R.pl_zwd_present is PASS B's record, so a
    // station whose tropo column was absent this epoch keeps `cols` without it.
    // Receiver clock in the local block: the station's receiver-clock (and ISB)
    // columns become the next block indices, appended after the iono columns and
    // before the ZWD so the ZWD keeps its last-row invariant.  Every per-column
    // solve below then gathers (N_rr⁻¹)_{c_a, rcol} at no extra cost.
    const Eigen::Index rxclk_row0 = static_cast<Eigen::Index>(cols.size());
    const bool rxclk_in_cols = rxclk_blk_on && !R.pl_rxclk.empty();
    if (rxclk_in_cols)
        for (const auto& t : R.pl_rxclk) cols.push_back(t.col);
    // Receiver code bias in the local block: every present column, carried or cold, so
    // each is stashed with its cross terms to the station's other columns.
    const Eigen::Index rxcb_row0 = static_cast<Eigen::Index>(cols.size());
    const bool rxcb_in_cols = rxcb_blk_on && !R.pl_cb.empty();
    if (rxcb_in_cols)
        for (const auto& t : R.pl_cb) cols.push_back(std::get<1>(t));
    // The eq34 receiver phase bias in the local block: every unpinned receiver phase-bias
    // column with a live phase row (PASS B's record, pl_rpb), carried or cold, so each is
    // stashed with its cross terms to the station's other columns.
    const Eigen::Index rxpb_row0 = static_cast<Eigen::Index>(cols.size());
    const bool rxpb_in_cols = rpb_blk_on && !R.pl_rpb.empty();
    if (rxpb_in_cols)
        for (const auto& t : R.pl_rpb) cols.push_back(std::get<1>(t));
    // The station's present phase-colour columns, after the receiver
    // biases and before the ZWD (which keeps its last-row slot).
    const Eigen::Index pc_row0 = static_cast<Eigen::Index>(cols.size());
    const bool pc_in_cols = ctx.knobs.pc_on && !R.pl_pc.empty();
    if (pc_in_cols)
        for (const PlPc& pp : R.pl_pc) cols.push_back(pp.col);
    const bool zwd_in_cols = zwd_blk_on && R.pl_zwd_present
                             && R.pl_zwd.col >= 0;
    if (zwd_in_cols) cols.push_back(R.pl_zwd.col);
    Eigen::VectorXd xr, vard;
    try {
        xr = est.reconstructStation(est_idx, x_s);
        if (!carry_cpl_cond)   // the coupling carry uses the pure conditional (N_rr⁻¹)_cc, not this marginal
            vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
    } catch (...) { return sb; }
    // Local block (network.phase_leveling_local_block or the float_kf vehicle): this
    // station-epoch's harvest cross-covariance block C(a,b) = (N_rr⁻¹)_{c_a c_b} over
    // `cols`.  One shared_ptr per station-epoch, filled row by row below from
    // each column's own n_c solve (the gather stationRegression already performs),
    // and referenced by every entry stashed at this harvest.  It is re-stashed
    // whole every epoch from the posterior, so Σ does not drift across epochs; the
    // previous block is released when its last referencing entry is replaced.
    // Null when the knob is off ⇒ nothing below allocates or writes.
    std::shared_ptr<PlHarvestBlock> hblk;
    if (pl_local_block && carry_cpl_cond) {
        hblk = std::make_shared<PlHarvestBlock>();
        const Eigen::Index nb = static_cast<Eigen::Index>(cols.size());
        hblk->C = Eigen::MatrixXd::Constant(nb, nb, std::numeric_limits<double>::quiet_NaN());
        hblk->stamp_s = pl_now_s;
        // The block takes the within-station half of the clones'
        // conditional covariance once every column is harvested.  No-op off.
        networkClockCloneHarvestBlock(ccl_h, est_idx, hblk, cols);
    }
    // ---- The station's layout, recorded once ----
    sb.ok   = true;
    sb.span = harvestBlockSpans(iono_row0, rxclk_row0, rxcb_row0, rxpb_row0, pc_row0,
                                static_cast<Eigen::Index>(cols.size()), zwd_in_cols);
    sb.cols = std::move(cols);
    sb.xr   = std::move(xr);
    sb.vard = std::move(vard);
    sb.blk  = std::move(hblk);
    return sb;
}

void networkHarvestBorderEnd(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    double pl_now_s = ep.clock.pl_now_s;
    ClockCloneHarvest& ccl_h = border.ccl_h;
    SharedHistoryHarvest& shx_h = border.shx_h;
    // ---- End of the bindings ----
    // Every carried column of every station is harvested; add the clones'
    // within-station conditional covariance to the harvest blocks.  No-op off.
    networkClockCloneHarvestEnd(ccl_h, ctx);
    // With every carried row of the network harvested, compress the rows'
    // shared conditional term into the history and add each row's remainder (the clone
    // end above returns at once when the history is on).  No-op off.
    networkSharedHistoryHarvestEnd(shx_h, ccl_h, ctx, pl_now_s);
}

std::array<BlockSpan, kBlockSegmentCount> harvestBlockSpans(
    Eigen::Index iono_row0, Eigen::Index rxclk_row0, Eigen::Index rxcb_row0,
    Eigen::Index rxpb_row0, Eigen::Index pc_row0, Eigen::Index n_cols, bool zwd_in_cols) {
    const Eigen::Index z = n_cols - (zwd_in_cols ? 1 : 0);
    return {{BlockSpan{0, iono_row0},
             BlockSpan{iono_row0, rxclk_row0 - iono_row0},
             BlockSpan{rxclk_row0, rxcb_row0 - rxclk_row0},
             BlockSpan{rxcb_row0, rxpb_row0 - rxcb_row0},
             BlockSpan{rxpb_row0, pc_row0 - rxpb_row0},
             BlockSpan{pc_row0, z - pc_row0},
             BlockSpan{z, zwd_in_cols ? Eigen::Index{1} : Eigen::Index{0}}}};
}

} // namespace GnssProcessor
