/**
 * @file NetworkLocalCarryHarvest.cpp
 * @brief @internal The station-local carries: their post-solve harvest, stash and prune.
 *
 *   networkHarvestLocalCarries    the harvest, stash and prune of the station-local
 *                                 carries from the solved station blocks, called by
 *                                 networkLeveling (filter/NetworkLeveling.cpp) after the
 *                                 epoch's solve, and directly by a unit test.
 * Card NOM3 step HV1 is planned to cut the harvest by carried kind.
 *
 * networkHarvestLocalCarries opens with an alias block (decomposition INC-4) that binds
 * the driver's former local names to members of the context, so the relocated code keeps
 * those names.
 *
 * One of the seven files that define the network run context's functions: card NOM3 step
 * HV0 split them out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/app/NetworkRunContext.hpp.
 */

#include "app/NetworkRunContext.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, traitsOf, SignalId, nominalCarrierFrequencyHz
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkAmbStateTrace.hpp"   // AmbStateRow / ambStateNoteErase (RF-1 §7.4)
#include "filter/NetworkAmbFloat.hpp"       // ambFloatBirthFloor (RF-1 step 4)
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkParamInventory.hpp" // rxIfbFamilyReseeded (card L5B)
#include "filter/NetworkPriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkSbasis.hpp"          // NetworkSbasis::Plan
#include "filter/StateKeySchema.hpp"         // StateKeys: ambiguity and phase-bias keys, GLONASS IFB keys

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GnssProcessor {

void networkHarvestLocalCarries(NetworkRunContext& ctx, const GnssConfig& config_,
                                NetworkEstimator& est, const Eigen::VectorXd& x_s,
                                std::vector<NetStation>& stations,
                                const std::vector<int>& est_index_of,
                                const std::vector<StationAssembly>& asm_res,
                                const NetworkSbasis::Plan& plan,
                                const SphericalHarmonicIonosphere* sh_model,
                                const SphericalHarmonicIonosphere* sh_model2,
                                NetworkCarryVce& carry_vce_est,
                                NetworkPriorResidual& prires_est, bool carry_warm,
                                double pl_now_s,
                                const std::vector<std::string>& registered_clock_sats,
                                const std::set<std::string>& shared_phase_keys,
                                const std::vector<std::pair<std::string, std::string>>& registered_code_keys) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& rpb_carry = ctx.carry.rpb_carry;
    auto& eq34_reref = ctx.carry.eq34_reref;
    auto& n_cpl_harvest_fail = ctx.counters.n_cpl_harvest_fail;
    auto& n_cpl_stash_empty = ctx.counters.n_cpl_stash_empty;
    auto& n_cpl_stash = ctx.counters.n_cpl_stash;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   carry_vce = ctx.knobs.carry_vce;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double pl_q_amb = ctx.knobs.pl_q_amb;
    const double pl_var_floor = ctx.knobs.pl_var_floor;
    const bool   pl_local_block = ctx.knobs.pl_local_block; // stash the harvest cross-covariance block beside the per-column carry (F3)
    // Birth-prior floor that the stash records in LocalCarry::var_birth (RF-1 §3.3(b)).
    // 0.0 (default) ⇒ var_birth == var_first to the ULP, so the floor has no effect.
    const double amb_birth_var_floor = ctx.knobs.amb_birth_var_floor;
    // The σ-power of the birth floor's elevation mapping is the code weights' power,
    // because the floor states what a code-determined level is worth (RF-1 step 4,
    // design §3.3(b)).
    const double amb_elev_power = config_.getCodeElevationPower();
    // Per-arc carry-state trace (network.trace_amb_state, RF-1 §7.4).  The stride gates
    // trajectory rows only; an event (birth / drop / erase) is written on every epoch,
    // so the trace is a complete inventory of events.  Off, the whole block below is
    // unreachable (byte-identical).
    const bool amb_state_on = ctx.knobs.trace_amb_state;
    const bool amb_state_tr =
        amb_state_on
        && (ctx.knobs.trace_amb_state_every <= 1
            || (static_cast<long long>(ctx.counters.n_loop_epochs)
                % ctx.knobs.trace_amb_state_every) == 0);
    // Age in epochs for the trace's relevel clock uses the run's own observed
    // inter-epoch spacing, so the column means the same thing at 1 s and 30 s cadence
    // without a nominal rate.  It is read here, not later: ctx.carry.pl_prev_now_s is
    // advanced to this epoch at the end of this function, so inside the stash loop it
    // still holds the previous stamp.
    const double amb_state_dt_ep_s = [&] {
        const double dt = pl_now_s - ctx.carry.pl_prev_now_s;
        return (std::isfinite(dt) && dt > 0.0 && dt <= pl_max_gap_s) ? dt : 0.0;
    }();
    const bool   zwd_on = ctx.knobs.zwd_on;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // Prior-residual census: the pl / zwd / rxcb feed arms.  Resolved once at run start
    // into per-family bools, so an off run pays one bool test per harvest loop and
    // nothing else.
    const bool   prires_pl   = ctx.knobs.prires_pl;
    const bool   prires_zwd  = ctx.knobs.prires_zwd;
    const bool   prires_rxcb = ctx.knobs.prires_rxcb;
    // There is no `zwd_q` alias here, on purpose (RS-10b, §21.21): this body's one
    // consumer (the prior-residual census's fold_var) reads the effective q through
    // NetworkKnobs::zwdQEff(zwd_seam_live), because inside a live seam window an armed
    // network.zwd_seam_reseed_q replaces network.zwd_carry_q at the injector.  A bare
    // zwd_q would mis-scale the `zwd` family's σ̂₀ without any warning.
    // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): the ZWD column
    // joins the F3 harvest block.  It is gated on the same live predicate as the block
    // itself (pl_local_block && carry_cpl_cond), so the harvest and the inject agree on
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
    const double rxclk_gap_s   = ctx.knobs.rxclk_max_gap_s;
    const double rxclk_q       = ctx.knobs.rxclk_q_m2;
    const double rxclk_rate_q  = ctx.knobs.rxclk_rate_q;
    const double rxclk_rate_var0 = ctx.knobs.rxclk_rate_var0;
    const double rxclk_jump_k  = ctx.knobs.rxclk_jump_k;
    // RXCB-IN-BLOCK (network.carry_rx_code_bias_in_block, card CTU section 14): the
    // receiver code-bias columns join the same harvest block and are stashed with their
    // border regression and block row here; the standalone diagonal harvest below then
    // skips its stash.  Off => `cols` is untouched (byte-identical).
    const bool   rxcb_blk_on   = ctx.knobs.rxcb_blk_on;
    // Seam-scoped ZWD re-seed + unconditional Z-row capture (§17 finding 3).
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;

    // --- Two-edge continuous handover: ledger harvest --------------------------
    //
    // Record, for every anchor-candidate column, the station's absolute ambiguity
    // estimate in this epoch's realized frame, so that if the anchor for that key moves
    // next epoch the election can pin the successor at this value instead of at 0,
    // which makes the gauge change an S-transform rather than a re-realization.
    //
    // The election cannot read `st.sm->getStateVector()(amb_idx)` instead: that is
    // `x_lin`, a linearization frame.  The network solve is never written back into the
    // per-station StateManager (`setStateVector` runs once, at setup), and an `x_lin`
    // shift on a local column is an exact solve no-op, so pinning to it would carry no
    // information about where the arc sits.  The estimate is `x_lin + back-substituted
    // Δx`, which exists only here, after the solve.  Hence the one-epoch deferral:
    // harvested now, consumed by the next epoch's election, in the same way as
    // carry_innov_pending.
    //
    // The population is `R.qc_amb`, the carry-neutral census: columns present in A_r
    // and not pinned.  A station holding the pin is not a handover target, and its
    // pinned column carries no independent information.  Not gated on carry_warm, on
    // purpose: the ledger has to work on lineages where the phase-leveling carry does
    // not exist.
    if (ctx.knobs.pd_handover_continuous) {
        ctx.carry.handover.value_abs.clear();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            if (R.qc_amb.empty()) continue;
            const std::size_t est_idx_hv = static_cast<std::size_t>(est_index_of[i]);
            Eigen::VectorXd xr, vhv;
            std::vector<Eigen::Index> cols_hv;
            cols_hv.reserve(R.qc_amb.size());
            for (const auto& pa : R.qc_amb) cols_hv.push_back(pa.col);
            try {
                xr  = est.reconstructStation(est_idx_hv, x_s);
                // The pin is a hard constraint (weight ~1e10), so the re-coupling edge
                // has to re-validate the value before applying it (invariant #7).  This
                // uses the border-exact variance, not the leveling carry's
                // border-inclusive harvest: the shared phase-bias column's uncertainty
                // moves the pin target and the realized frame together (common mode in
                // the handover step), so including it gates the wrong quantity in both
                // directions, refusing settled successors while passing a large real
                // error (§5.0's G21 case).  The ceiling has to bound the successor's
                // error relative to the shared frame, which is what this returns.
                vhv = est.pureConditionalLocalVarianceDiag(est_idx_hv, cols_hv);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < R.qc_amb.size(); ++a) {
                const PlAmb& pa = R.qc_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                const double var = (a < static_cast<std::size_t>(vhv.size()))
                                       ? vhv(static_cast<Eigen::Index>(a)) : -1.0;
                // Key on "sat_signal", byte-identical to the election's `amb`
                // (kAmbiguityDelimiter is '_'), so the lookup needs no translation.
                AnchorHandoverLedger::Entry hv_e{value_abs, var};
                // network.phase_datum_handover_max_var_ratio: the arc's first-stash
                // conditional variance (0 = never carried ⇒ the ratio test refuses it),
                // read from the previous epoch's carry (this harvest precedes the leveling
                // harvest).
                {
                    const auto lcit = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    if (lcit != local_carry.end()) hv_e.var_first = lcit->second.var_first;
                }
                ctx.carry.handover.value_abs[{static_cast<int>(i),
                                              StateKeys::makeAmbiguityKey(pa.sat, pa.signal)}] = hv_e;
            }
        }
        ctx.carry.handover.primed = true;
    }

    // --- Phase-leveling carry: harvest posterior and stash (increment 1) ----
    // For each present non-pivot station, recover its ambiguities' absolute values
    // (x_lin + back-substituted Δx) and their conditional variances (the cheap
    // per-column diagonal harvest, not the full per-station inverse), and refresh the
    // persistent carry.  Then drop entries unseen beyond the max gap (a stale-carry and
    // map-growth guard, distinct from the slip-driven refloat/drop of later increments).
    if (carry_warm) {
        // Build, once per epoch, the shared column → identity descriptor map and the
        // datum-frame marginal-variance diagonal, so each carried local's regression on
        // the free shared border can be stashed in a layout-stable (identity-keyed) form.
        // Empty and unused on the legacy diagonal path → byte-identical.
        std::map<Eigen::Index, CoupledShared> col2desc;
        Eigen::MatrixXd cov_df;   // shared covariance, datum-pinned + ensemble-gauge cols zeroed
        // Card SCL: the clones' posterior regression on the rest of the border, once per
        // epoch (filter/NetworkClockClone).  Inert off and when no clone is registered.
        ClockCloneHarvest ccl_h;
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
        }
        // Card SCL2 X2: where each harvested row's entry is written, for the compression at
        // the end (filter/NetworkSharedHistory).  Inert off.
        SharedHistoryHarvest shx_h = networkSharedHistoryHarvestBegin(ctx, ccl_h);
        // Harvest one carried column's pure conditional variance (N_rr⁻¹)_cc and its
        // regression on the free shared border (identity-keyed, sparsified).
        // Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
        // With F3 (network.phase_leveling_local_block), non-null `cross_cols` / `cross`
        // make the same per-column solve also read the station's other carried columns
        // and return them as (N_rr⁻¹)_{c c_j} (a gather, no second solve); both are null
        // on every other call (byte-identical).
        auto harvestCoupling = [&](std::size_t est_idx, Eigen::Index col,
                                   const std::vector<Eigen::Index>* cross_cols,
                                   Eigen::VectorXd* cross)
                -> std::pair<double, std::vector<CoupledShared>> {
            NetworkNormalEquations::StationRegression reg;
            try { reg = est.stationRegression(est_idx, col, /*carry_aware=*/false, cross_cols); }
            catch (...) { return {-1.0, {}}; }
            if (cross) *cross = std::move(reg.cross);
            // Card SCL: the row's coefficients on the clock clones regressed onto the rest of
            // the border and the clones' conditional variance added (the clone columns are
            // not in col2desc, so without this the loop below would drop them).  No-op off.
            networkClockCloneHarvestRow(ccl_h, ctx, est_idx, col, reg.row, reg.cond_var);
            std::vector<CoupledShared> cpl;
            const Eigen::Index ns = reg.row.size();
            if (ns > 0) {
                const double maxabs = reg.row.cwiseAbs().maxCoeff();
                const double thr = kCouplingSparsifyRelThresh * maxabs;
                for (Eigen::Index j = 0; j < ns; ++j) {
                    const double mj = reg.row(j);
                    if (std::abs(mj) <= thr) continue;          // sparsify near-zero entries
                    const auto dit = col2desc.find(j);
                    if (dit == col2desc.end()) continue;        // col not in the enumerated shared set
                    // A datum-pinned or ensemble-gauge column is kept in the row
                    // (coefficient and its ŝ_prev RHS term) with p_prev = 0 ("exactly
                    // known"; the zeroed cov_df diagonal mirrors
                    // conditionalLocalVarianceDiag's frame).  The full regression row is
                    // exactly gauge-invariant: every station data row satisfies
                    // A_s·g_s + A_r·g_r = 0 for each structural gauge direction, so
                    // M = e_cᵀN_rr⁻¹N_rs has Mᵀg_s = −g_r(c), and the row's +1 local
                    // completes the cancellation.  Dropping the pinned columns'
                    // coefficients would leave M'ᵀg ≠ 0: every coupling row would then
                    // observe the clock gauge, and the W5 null-space gate would see "0 null
                    // directions vs declared dim" and withhold every epoch that carries
                    // rows, which leaves the coupling carry inert while the A/B still
                    // reads byte-identical.  Under the datum the pinned term is exact
                    // (Dirichlet elimination), and p_prev = 0 degrades every other path
                    // correctly: the κ time update falls through unshrunk (a pinned column
                    // does not move), and a departed pinned column marginalizes to +0.
                    const double pj = (j < cov_df.rows() && cov_df(j, j) > 0.0)
                                          ? cov_df(j, j) : 0.0;
                    CoupledShared cs = dit->second;
                    cs.coeff  = mj;
                    cs.s_prev = x_s(j);
                    cs.p_prev = pj;
                    cpl.push_back(std::move(cs));
                }
            }
            return {reg.cond_var, std::move(cpl)};
        };
        // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): the ZWD
        // harvest is a separate loop further down (it has to run for stations with no
        // carried ambiguities), so the block this loop allocates, and the `cols` order its
        // rows are indexed by, are handed forward per station.  Empty vectors when the
        // knob is off, and nothing below touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> zblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       zblk_cols_of_sta;
        if (zwd_blk_on) {
            zblk_of_sta.resize(stations.size());
            zblk_cols_of_sta.resize(stations.size());
        }
        // Iono in the local block: the same hand-forward for the per-LOS iono harvest
        // loop below.  `iblk_row0_of_sta` is the block index of this station's first iono
        // column; the iono columns are appended right after the ambiguities and before
        // the ZWD, so the ZWD keeps the last-row slot that its own (validated) card
        // computes as C.rows()−1.  Empty vectors when the knob is off; nothing below
        // touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> iblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       iblk_cols_of_sta;
        std::vector<Eigen::Index>                    iblk_row0_of_sta;
        if (iono_blk_on) {
            iblk_of_sta.resize(stations.size());
            iblk_cols_of_sta.resize(stations.size());
            iblk_row0_of_sta.assign(stations.size(), -1);
        }
        // Receiver clock in the local block: the same hand-forward for the
        // receiver-clock harvest loop.  `rblk_row0_of_sta` is the block index of this
        // station's first clock-family column, appended after the iono columns and before
        // the ZWD, so the ZWD keeps the last-row slot that its own (validated) card
        // computes as C.rows()−1.
        std::vector<std::shared_ptr<PlHarvestBlock>> rblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       rblk_cols_of_sta;
        std::vector<Eigen::Index>                    rblk_row0_of_sta;
        if (rxclk_blk_on) {
            rblk_of_sta.resize(stations.size());
            rblk_cols_of_sta.resize(stations.size());
            rblk_row0_of_sta.assign(stations.size(), -1);
        }
        // RXCB-IN-BLOCK: the same hand-forward for the receiver code-bias loop below.
        std::vector<std::shared_ptr<PlHarvestBlock>> cblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       cblk_cols_of_sta;
        std::vector<Eigen::Index>                    cblk_row0_of_sta;
        if (rxcb_blk_on) {
            cblk_of_sta.resize(stations.size());
            cblk_cols_of_sta.resize(stations.size());
            cblk_row0_of_sta.assign(stations.size(), -1);
        }
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            // Prioritized datum: harvest the former pivot too.  Its non-anchored arcs are
            // in R.pl_amb, and anchored (pinned) keys were excluded at build, so inject
            // and harvest partition on the same per-(sat,signal) predicate.
            if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
            const StationAssembly& R = asm_res[i];
            // Iono in the local block: a station with no carry-eligible ambiguities can
            // still have live per-LOS iono columns, and PASS B builds a block for that
            // case (its `if (!pl_pend.empty() || !iono_bc.empty())` gate).  Skipping it
            // here would stash those entries without a block, so every pair with them
            // would be zeroed next epoch, giving a diagonal iono prior (the vehicle the
            // in-block carry replaces).  Harvest and inject have to agree on which
            // station-epochs have a block.  Off ⇒ iono_blk_on is false and this reduces
            // to `if (R.pl_amb.empty()) continue;` (byte-identical).
            // Receiver clock in the local block: the same holds for a station whose only
            // live carry is its receiver clock.  PASS B builds a block for that case too,
            // and skipping it here would stash the entry without a block, diagonalizing
            // the most station-common direction of all.
            if (R.pl_amb.empty() && !(iono_blk_on && !R.pl_iono.empty())
                && !(rxclk_blk_on && !R.pl_rxclk.empty())
                && !(rxcb_blk_on && !R.pl_cb.empty())
                && !(ctx.knobs.pc_on && !R.pl_pc.empty())) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_amb.size() + 1 + R.pl_iono.size() + R.pl_rxclk.size());
            for (const auto& a : R.pl_amb) cols.push_back(a.col);
            // Iono in the local block: the station's present per-LOS iono columns become
            // block indices n_amb … n_amb+k−1, appended before the ZWD so the ZWD keeps
            // its invariant that the appended column is the last row.  Every per-column
            // solve below then gathers (N_rr⁻¹)_{c_a, icol} at no extra cost, the same
            // gather that already covers the ambiguity–ambiguity terms.  R.pl_iono is
            // PASS B's record of what it pushed, so a station whose block was never built
            // keeps `cols` as it was.
            const Eigen::Index iono_row0 = static_cast<Eigen::Index>(cols.size());
            const bool iono_in_cols = iono_blk_on && !R.pl_iono.empty();
            if (iono_in_cols)
                for (const auto& t : R.pl_iono) cols.push_back(std::get<1>(t));
            // ZWD in the local block (B9-in-block): the station's ZWD column becomes block
            // index n (the last entry), so every per-column solve below gathers its
            // (N_rr⁻¹)_{c_a, zcol} cross term at no extra cost, the same gather that
            // already covers the ambiguity-ambiguity terms.  It is appended after the
            // ambiguity columns so the pl rows keep their indices `a`; the ZWD loop below
            // writes the ZWD's own row n.  R.pl_zwd_present is PASS B's record, so a
            // station whose tropo column was absent this epoch keeps `cols` without it.
            // Receiver clock in the local block: the station's receiver-clock (and ISB)
            // columns become the next block indices, appended after the iono columns and
            // before the ZWD so the ZWD keeps its last-row invariant.  Every per-column
            // solve below then gathers (N_rr⁻¹)_{c_a, rcol} at no extra cost.
            const Eigen::Index rxclk_row0 = static_cast<Eigen::Index>(cols.size());
            const bool rxclk_in_cols = rxclk_blk_on && !R.pl_rxclk.empty();
            if (rxclk_in_cols)
                for (const auto& t : R.pl_rxclk) cols.push_back(t.col);
            // RXCB-IN-BLOCK: every present receiver code-bias column, carried or cold, so
            // each is stashed with its cross terms to the station's other columns.
            const Eigen::Index rxcb_row0 = static_cast<Eigen::Index>(cols.size());
            const bool rxcb_in_cols = rxcb_blk_on && !R.pl_cb.empty();
            if (rxcb_in_cols)
                for (const auto& t : R.pl_cb) cols.push_back(std::get<1>(t));
            // Card LGM: the station's present phase-colour columns, after the receiver
            // code bias and before the ZWD (which keeps its last-row slot).
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
                if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc, not this marginal
                    vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            // Local block (F3, network.phase_leveling_local_block): this station-epoch's
            // harvest cross-covariance block C(a,b) = (N_rr⁻¹)_{c_a c_b} over R.pl_amb's
            // columns.  One shared_ptr per station-epoch, filled row by row below from
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
                // ZWD in the block: hand the block and its column order to the ZWD loop
                // below, which owns row n.  Handed over only when the ZWD is in `cols`;
                // otherwise the ZWD loop would write a row that does not exist.
                if (zwd_in_cols) {
                    zblk_of_sta[i]      = hblk;
                    zblk_cols_of_sta[i] = cols;
                }
                // Iono in the block: same hand-forward, plus the row offset the iono loop needs.
                if (iono_in_cols) {
                    iblk_of_sta[i]      = hblk;
                    iblk_cols_of_sta[i] = cols;
                    iblk_row0_of_sta[i] = iono_row0;
                }
                // Receiver clock in the block: same hand-forward, plus its loop's row offset.
                if (rxclk_in_cols) {
                    rblk_of_sta[i]      = hblk;
                    rblk_cols_of_sta[i] = cols;
                    rblk_row0_of_sta[i] = rxclk_row0;
                }
                if (rxcb_in_cols) {   // RXCB-IN-BLOCK
                    cblk_of_sta[i]      = hblk;
                    cblk_cols_of_sta[i] = cols;
                    cblk_row0_of_sta[i] = rxcb_row0;
                }
                // Card SCL: the block takes the within-station half of the clones'
                // conditional covariance once every column is harvested.  No-op off.
                networkClockCloneHarvestBlock(ccl_h, est_idx, hblk, cols);
            }
            for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                const PlAmb& pa = R.pl_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                Eigen::VectorXd cross;   // (N_rr⁻¹)_{c_a c_b} over the station's carried columns, empty when off (F3)
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, pa.col, hblk ? &cols : nullptr,
                                              hblk ? &cross : nullptr);
                    var = hc.first; coupling = std::move(hc.second);
                    // Coupling observability (cards 1 and 1d): a failed harvest drops the
                    // carry entirely; an empty regression degrades the row to a plain
                    // diagonal, which only this counter shows.
                    if (!(var > 0.0) || !std::isfinite(var)) ++n_cpl_harvest_fail;
                    else if (coupling.empty())               ++n_cpl_stash_empty;
                    else                                     ++n_cpl_stash;
                } else {
                    var = (a < static_cast<size_t>(vard.size())) ? vard(a) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                // LS-VCE monitor (M2): fold this ambiguity carry's post-fit innovation
                // (carried prior − freshly solved) into the monitor before the stash
                // overwrites the prior.  Only rows that carried a prior this epoch
                // qualify, mirroring the inject predicate: a live prior within max-gap,
                // not a slip or refloat (pending reseed), and not a card-3 re-referenced
                // arc (a frame step, not noise).
                // Prior-residual census: the same (prior − solved, gap, post_var,
                // fold_var) tuple the VCE monitor folds, but without its two exclusions,
                // which become the RESPONDED / FRAME labels; that population is what a
                // detector exists to find and what a variance component should drop.
                // Hoisted above the monitor so both read one qualification.
                //
                // `folded` is reproduced from the inject predicate, not mirrored by hand:
                // every clause of the PASS-B injector is a pure function of values still
                // in scope here (the carry entry, the gap, the pending-reseed mark, which
                // is cleared only after this point, pa.x_lin and var_eff).  An error here
                // matters: the folded branch on an unfolded row inflates |t| about 3000×
                // through the ε clamp and would make the positive control pass
                // vacuously.  Two clauses cannot be reproduced here and are assumed to
                // hold, since the ambiguity reached R.pl_amb: the gauge-pin exclusion and
                // the h2local column lookup.  eq34_reref is a frame label for pl, not a
                // folded=false condition: the pl injector has no eq34 skip, so a
                // re-referenced arc did fold.
                const bool prires_want = prires_pl && prires_est.enabled();
                if (carry_vce || prires_want) {
                    const auto oc = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    const bool reseed_pending =
                        stations[i].sm->isAmbiguityPendingReseed(pa.amb_key);
                    const bool reref = eq34_reref.count({pa.sat, pa.signal}) > 0;
                    // Harvest guard (SPEC_slip_relevel_walk): a freed or bridged arc's
                    // (prior − solved) innovation is slip-scale, and without this guard
                    // it would be ingested as a clean row (the PASS-B pre-pass consumed
                    // the detector's own mark, so reseed_pending is false here for every
                    // slip the detector caught).
                    const bool relevel_freed =
                        ctx.carry.relevel.free_ambs.count({static_cast<int>(i), pa.amb_key}) > 0;
                    const bool relevel_bridged =
                        ctx.carry.relevel.bridged.count(
                            {static_cast<int>(i), pa.sat, pa.signal}) > 0;
                    if (oc != local_carry.end()) {
                        const double gapv = pl_now_s - oc->second.last_seen_s;
                        if (gapv >= 0.0 && gapv <= pl_max_gap_s) {
                            const double fold_var_hand =
                                [&] {
                                    double v = oc->second.var + pl_q_amb * std::max(0.0, gapv);
                                    // Under F3 (phase_leveling_local_block) a column carried
                                    // in a whitened block folded with the block's joint Σ,
                                    // whose diagonal for this column is this hand value
                                    // (plus the card-1d / departed terms neither path
                                    // mirrors), so the mirror reports Σ's diagonal, the
                                    // column's marginal prior variance.  This is
                                    // deliberate: the census standardizes e by the marginal
                                    // prior σ the column carried.
                                    if (pl_var_floor > 0.0) v = std::max(v, pl_var_floor);
                                    return v;
                                }();
                            // VCE apply (M2 v2): the PASS-B injector multiplies the same
                            // hand value by the same α² (the factor table is refreshed once
                            // per epoch, at the end, so inject and harvest read one value),
                            // so `fold_var` here is the variance the fold used.  Both
                            // consumers need that: the census standardizes by the used
                            // weight, and the monitor's sigma_hat_post reads 1 at
                            // convergence only if its denominator is the used weight too.
                            // The hand value is passed alongside because it is the factor
                            // iteration's own denominator and has to stay independent of
                            // α².  The factor is 1 when off.
                            const double fold_var =
                                fold_var_hand * ctx.carry.vce_apply.factor(
                                                    NetworkCarryVceApply::kPl, pa.sat);
                            // Open issue (a separate card): the census numerator.  `innov`
                            // is the raw carried-minus-solved difference.  Under the card-1
                            // conditional coupling the carried prior's RHS is not
                            // `value_abs` alone: the coupling rows add Σ M_rj·ŝ_prev,j to
                            // it (the `rhs += cs.coeff * cs.s_prev` accumulation in
                            // NetworkCoupling.cpp), so on a coupled posture this ê and the
                            // ê the fold saw differ by the regressor term.  The denominator
                            // side of the same mismatch produces the r_negative population
                            // (post_var > fold_var, because the harvested posterior is a
                            // conditional variance), which the census re-partitions into
                            // its own RNEG class instead of letting it head the ladder.
                            // Aligning the numerator is not part of that change, on
                            // purpose: it would move every coupled family's ê and therefore
                            // its σ̂₀, which is solve-adjacent measurement work with its own
                            // A/B.
                            const double innov = oc->second.value_abs - value_abs;
                            if (carry_vce && !reseed_pending && !reref
                                && !relevel_freed && !relevel_bridged)
                                carry_vce_est.addRow("pl", pa.sat, innov,
                                                     gapv, var, fold_var, fold_var_hand);
                            if (prires_want) {
                                NetworkPriorResidual::Input in;
                                in.family   = "pl";
                                in.sat      = pa.sat;
                                in.signal   = pa.signal;
                                in.station  = static_cast<int>(i);
                                in.e        = innov;
                                in.fold_var = fold_var;
                                in.post_var = var;
                                in.gap_s    = gapv;
                                // The inject predicate, reproduced (see above).  A freed
                                // arc's inject was skipped (the latched set); a bridged
                                // arc's (shifted) prior did fold.
                                in.folded   = !reseed_pending && !relevel_freed
                                           && std::isfinite(pa.x_lin)
                                           && fold_var > 0.0;
                                in.label = (reseed_pending || relevel_freed || relevel_bridged)
                                               ? PriorResidualLabel::Responded
                                         : reref ? PriorResidualLabel::Frame
                                                 : PriorResidualLabel::Clean;
                                in.mech  = reseed_pending   ? "reseed"
                                         : relevel_bridged  ? "delta_bridge"
                                         : relevel_freed    ? "relevel_free"
                                         : reref            ? "eq34_reref" : "";
                                prires_est.addRow(in);
                            }
                        }
                    }
                }
                // Observatory (§18.12, in the measure-first form of
                // SPEC_lane_slip_response §7.3): at the first post-resume harvest the
                // restored regressor and this epoch's freshly derived one are both in hand,
                // and their agreement is the only direct evidence that the restored row
                // was frame-valid.  In the algebra, a row with A_r = e_c, A_s = M
                // contributes α(M − M_d)(M − M_d)ᵀ to the reduced shared normals and
                // α(y − β)(M − M_d) to the shared RHS, so M == M_d makes the row exactly
                // shared-neutral, and rel ≪ 1 shows that it was.  M == 0 (the diagonal
                // restore) is the maximal deviation.  Nothing is written back:
                // byte-identical on every solve artifact.
                if (ctx.carry.resume_cpl_census_pending && carry_cpl_cond
                    && !coupling.empty()) {
                    const auto old = local_carry.find(std::make_tuple(
                        static_cast<int>(i), pa.sat, pa.signal));
                    if (old != local_carry.end() && !old->second.coupling.empty()) {
                        const auto idOf = [](const CoupledShared& c) {
                            return std::make_tuple(static_cast<int>(c.kind), c.sat,
                                                   c.signal, c.n, c.m,
                                                   static_cast<int>(c.trig), c.sh_layer);
                        };
                        std::map<decltype(idOf(coupling.front())), double> now;
                        double maxabs = 0.0, rhs_new = 0.0, rhs_old = 0.0, dmax = 0.0;
                        for (const CoupledShared& cs : coupling) {
                            now[idOf(cs)] = cs.coeff;
                            maxabs  = std::max(maxabs, std::abs(cs.coeff));
                            rhs_new += cs.coeff * cs.s_prev;
                        }
                        long long matched = 0, gone = 0;
                        for (const CoupledShared& cs : old->second.coupling) {
                            rhs_old += cs.coeff * cs.s_prev;
                            const auto h = now.find(idOf(cs));
                            if (h == now.end()) { ++gone; continue; }
                            ++matched;
                            dmax = std::max(dmax, std::abs(cs.coeff - h->second));
                        }
                        ++ctx.counters.n_resume_cpl_census_rows;
                        ctx.counters.n_resume_cpl_census_terms     += matched;
                        ctx.counters.n_resume_cpl_census_unmatched += gone;
                        if (maxabs > 0.0)
                            ctx.carry.resume_cpl_census_rel.push_back(dmax / maxabs);
                        ctx.carry.resume_cpl_census_rhs.push_back(
                            std::abs(rhs_new - rhs_old));
                    }
                }
                {
                    // First-stash conditional variance: set once at the arc's birth and
                    // kept across re-stashes (the σ-scale denominator of the handover ratio
                    // ceiling; an erase-and-rebirth starts it over, and a re-level keeps it,
                    // so a refloated arc reads as unconverged until it re-earns the level).
                    const auto lkey = std::make_tuple(static_cast<int>(i), pa.sat, pa.signal);
                    const auto oldc = local_carry.find(lkey);
                    const double var_first =
                        (oldc != local_carry.end() && oldc->second.var_first > 0.0)
                            ? oldc->second.var_first : var;
                    // The arc's birth, birth_ep (the clock of the F3 block admission age
                    // gate and of the candidacy bar) and its seconds twin birth_s (the hard
                    // window floor of the sim-only slip oracle: a truth slip stamped at or
                    // before the entry's creation is already absorbed by the level the
                    // harvest created and is not re-applied), is stamped by
                    // localCarryStampBirth below, the single definition of the
                    // first-stash-wins rule (filter/NetworkRunTypes.hpp).  The first stash
                    // wins, the stamp is kept across re-stashes, and an erase-and-rebirth
                    // restarts it (with no extra clause: the entry is gone, so
                    // `oldc == end()` and this epoch is the stamp).
                    // Its "already stamped" test is `!= 0`, not `> 0` (SPEC_event_harness
                    // §15.4): a warm-resume-restored entry carries
                    // LocalCarry::kBirthEpRestored / kBirthSRestored so the rule can tell
                    // "restored, true age not snapshotted ⇒ mature" from "never stashed ⇒
                    // stamp it now".  With `> 0`, the first post-resume harvest would
                    // re-stamp every restored arc as born at the resume epoch, and every
                    // age gate would then see a network of newborns for min_age epochs.
                    // The re-level stamp (introduced for the retired young-arc inflation,
                    // SPEC_amb_iono_split_overcount §6.17–§6.18).  Two events, and only
                    // these two, produce a level whose value is code-quality while its
                    // harvested variance is phase-stiff:
                    //   * the entry is created: a first stash, or the re-stash after a T4
                    //     attribution erase or gap prune deleted the entry (the erase
                    //     path needs no separate clause because it deletes);
                    //   * the arc was freed this epoch, so this harvest re-levelled it
                    //     from one epoch of code.
                    // The freed predicate is the harvest's own `relevel_freed` above,
                    // re-read here because that one lives inside the VCE/census scope:
                    // `relevel.free_ambs` is the latched set the PASS-B inject consulted
                    // (empty when network.cycle_slip.relevel_free_epoch is off, so no knob
                    // guard is needed, the same rule the census uses).  Every other
                    // re-stash preserves the stamp, so the age keeps growing while the arc
                    // matures.  This is not birth_s: a re-level keeps birth_s (by design,
                    // the slip oracle's floor), and it is the re-level, not the birth,
                    // that resets what the level is worth.
                    const bool yng_created = (oldc == local_carry.end());
                    const bool yng_freed =
                        ctx.carry.relevel.free_ambs.count(
                            {static_cast<int>(i), pa.amb_key}) > 0;
                    LocalCarry nlc{value_abs, var, pl_now_s, std::move(coupling)};
                    nlc.var_first = var_first;
                    // The birth prior in force (RF-1 §3.3(b),
                    // docs/refound/RF-1_float_ambiguities_design.md): var_first floored by
                    // the posture's code-noise scale, set once at birth and kept across
                    // every re-stash and re-level as var_first is (an erase-and-rebirth
                    // restarts it, because the entry is gone).  The floor defaults to 0.0,
                    // which makes this var_first to the ULP; the drop (NetworkAmbFloat.cpp)
                    // restores max(var_birth, var_first, floor).  It is maintained here
                    // rather than at the drop site because what the arc was worth at birth
                    // is a property of the birth, and the harvest is the only place that
                    // sees an arc's first stash.
                    // The floor is elevation-mapped as the code row weights are
                    // (σ²(e) = σ²_zen / sin^{2p}(e), RF-1 step 4): a flat zenith floor would
                    // be optimistic at the low elevations that hold the metre-plus tail
                    // (§24.13 finding 2).  pa.elev_deg is −1 when the floor is unarmed or
                    // the station's modeled set had no elevation for this satellite, and
                    // ambFloatBirthFloor reads that as zenith.
                    const double amb_floor_los =
                        ambFloatBirthFloor(amb_birth_var_floor, pa.elev_deg,
                                           amb_elev_power);
                    nlc.var_birth =
                        (oldc != local_carry.end() && oldc->second.var_birth > 0.0)
                            ? oldc->second.var_birth
                            : std::max(var_first, amb_floor_los);
                    if (oldc == local_carry.end() && amb_floor_los > var_first)
                        ++ctx.counters.n_amb_birth_floored;
                    // The drop stamp (RF-1 §7.2) is written by the drop
                    // (NetworkAmbFloat.cpp) and carried forward unchanged by the harvest.
                    nlc.dropped_ep =
                        (oldc != local_carry.end()) ? oldc->second.dropped_ep : 0;
                    // Walk metric (RF-3a, A2).  This is the one site that sees the
                    // pre-fold carried value and the post-fold stash together, so it is
                    // where the amount the first fold after a drop absorbed can be
                    // measured.  Measure-only, maintained whenever the vehicle is armed and
                    // in both states of network.amb_float_drop_var_m2: an off run has to
                    // report its walk for the on run's number to be comparable (standing
                    // invariant #4).
                    nlc.drop_walk_ph =
                        (oldc != local_carry.end()) ? oldc->second.drop_walk_ph : 0;
                    if (nlc.drop_walk_ph > 0 && oldc != local_carry.end()) {
                        const double dv = std::abs(value_abs - oldc->second.value_abs);
                        if (std::isfinite(dv)) {
                            if (nlc.drop_walk_ph == 1)
                                ctx.counters.amb_walk_fold1_m.push_back(dv);
                            else
                                ctx.counters.amb_walk_fold2_m.push_back(dv);
                        }
                        nlc.drop_walk_ph = (nlc.drop_walk_ph == 1) ? 2 : 0;
                    }
                    localCarryStampBirth(
                        nlc, (oldc != local_carry.end() ? &oldc->second : nullptr),
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    nlc.relevel_s = localCarryStampRelevel(
                        yng_created ? 0.0 : oldc->second.relevel_s,
                        yng_created, yng_freed, pl_now_s);
                    // The arc's trajectory row (network.trace_amb_state, RF-1 §7.4),
                    // written from the one site that sees the pre-stash entry and the
                    // post-stash value together.  Measure-only: nothing below is read back,
                    // and when off the whole block is unreachable.
                    //
                    // `folded` / `route` / `fold_var` are not re-derived here: they are the
                    // record PASS B wrote into R.amb_state_fold at the site that emitted
                    // the row, so a joint-block fallback or a non-finite-RHS screen on u_r
                    // reads as what happened, not as what the knob posture implies.
                    // n_folds counts only the epochs in which a prior acted, so it cannot
                    // be derived from birth_ep: the epochs it skips (the free-epoch latch,
                    // the gap cliff, a pending reseed) are the ones the trace exists to
                    // show.
                    long long nlc_n_folds =
                        (oldc != local_carry.end()) ? oldc->second.n_folds : 0;
                    if (amb_state_on) {
                        const auto ff = R.amb_state_fold.find({pa.sat, pa.signal});
                        const bool did_fold = (ff != R.amb_state_fold.end());
                        if (did_fold) ++nlc_n_folds;
                        AmbStateRow row;
                        row.week    = static_cast<long long>(std::floor(pl_now_s / 604800.0));
                        row.tow     = std::fmod(pl_now_s, 604800.0);
                        row.station = stations[i].name;
                        row.sat     = pa.sat;
                        row.signal  = pa.signal;
                        row.value_abs_m  = value_abs;
                        row.var_m2       = var;
                        row.var_first_m2 = nlc.var_first;
                        row.var_birth_m2 = nlc.var_birth;
                        row.fold_var_m2  = did_fold ? ff->second.first : 0.0;
                        row.folded       = did_fold;
                        row.route        = did_fold ? ff->second.second : AmbFoldRoute::None;
                        row.n_folds      = nlc_n_folds;
                        // Age reads the stamps after localCarryStampBirth, so a restored
                        // arc reports the −1 sentinel instead of being re-stamped as a
                        // newborn (SPEC_event_harness §15.4), and an unstamped relevel
                        // clock reports −1 = "mature", not "age 0".
                        row.age_ep = (nlc.birth_ep > 0)
                            ? (static_cast<long long>(ctx.counters.n_loop_epochs) - nlc.birth_ep)
                            : nlc.birth_ep;
                        row.age_since_relevel_ep =
                            (nlc.relevel_s > 0.0 && amb_state_dt_ep_s > 0.0)
                                ? static_cast<long long>(
                                      std::llround((pl_now_s - nlc.relevel_s)
                                                   / amb_state_dt_ep_s))
                                : -1;
                        // The event vocabulary is design §7.4's, also used on the level
                        // vehicle for its response paths, so an RF-0 baseline and an RF-1
                        // arm compare row for row: a `drop:` here is an inject that did
                        // not happen because the response skipped it, which is what RF-1
                        // replaces with a bounded drop.  Precedence follows the inject's
                        // own order of tests.
                        const bool reseed_now =
                            stations[i].sm->isAmbiguityPendingReseed(pa.amb_key);
                        const double gap_now = (oldc != local_carry.end())
                            ? (pl_now_s - oldc->second.last_seen_s) : 0.0;
                        if (yng_created)          row.event = "birth";
                        else if (reseed_now)      row.event = "drop:reseed";
                        else if (yng_freed)       row.event = "drop:relevel_free";
                        else if (gap_now > pl_max_gap_s) row.event = "drop:stale_gap";
                        // The stride gates trajectory rows; an event row is always
                        // written, so D3's inventory is complete at any stride.
                        if (amb_state_tr || !row.event.empty())
                            ctx.carry.amb_state_rows.push_back(std::move(row));
                    }
                    nlc.n_folds = nlc_n_folds;
                    local_carry[lkey] = std::move(nlc);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pa.col, SharedHistoryMap::Amb,
                                                  static_cast<int>(i), pa.sat, pa.signal);   // card SCL2 X2; no-op off
                }
                if (hblk && cross.size() == hblk->C.rows()) {
                    // Local block (F3): row a of the station's harvest block is this
                    // column's n_c read at every carried column (its own entry equals
                    // `var`); the entry points at the block and its index, so the next
                    // inject can pair it with any sibling stashed at this harvest, and
                    // with nothing else.
                    hblk->C.row(static_cast<Eigen::Index>(a)) = cross.transpose();
                    auto& ent = local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)];
                    ent.blk = hblk;
                    ent.blk_idx = static_cast<int>(a);
                }
                // One-shot IF-phase refloat: the fresh post-slip value is now stashed, so
                // drop any pending-reseed mark and let the next epoch resume normal carry
                // on the new arc.  Cleared only after a successful stash: a skipped
                // non-finite value keeps the mark, so the stale pre-slip carry is not
                // trusted again.  No-op when unmarked.
                stations[i].sm->clearPendingReseed(pa.amb_key);
            }
            // Card AIL (network.trace_amb_if, trace class): the ionosphere-free ambiguity's
            // variance before and after this epoch, per line of sight whose two bands are
            // both carried in the block.  Prior: the joint covariance the inject folded
            // (R.pl_blk.Sigma, rows keyed by local column); posterior: the harvest block
            // just filled (rows a of R.pl_amb).  Both are conditional on the border.  The
            // demeaned versions project out the station's common mode over the traced lines
            // of sight of the same constellation (the direction the free receiver clock and
            // that constellation's receiver biases leave to the code).  Reads only; nothing
            // the solve uses is written.
            if (ctx.knobs.amb_if_trace && hblk && !R.pl_amb.empty()) {
                // The ionosphere-free pair of a line of sight: the first two bands of its
                // constellation in RINEX digit order (the traits table: GPS L1/L2, Galileo
                // E1/E5a, QZSS L1/L2, SBAS L1/L5), with the IF coefficients from their
                // carriers.  BeiDou is not traced: its first two bands, B1C and B1I, are
                // 14 MHz apart and form no usable pair (the coefficient is about 55); its
                // pair is a band-role decision of card NOM step N3.  GLONASS is never
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
                        const auto lit = local_carry.find(std::make_tuple(
                            static_cast<int>(i), t.sat,
                            R.pl_amb[static_cast<std::size_t>(t.idx[0])].signal));
                        const long long age = (lit != local_carry.end() && lit->second.birth_ep > 0)
                            ? static_cast<long long>(ctx.counters.n_loop_epochs) - lit->second.birth_ep
                            : -1;
                        ctx.carry.amb_if_rows.push_back(fmt::format(
                            "{:.0f},{:.1f},{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e}\n",
                            t_week, t_tow, stations[i].name, t.sat,
                            age, n, Vpr(r2, r2), Vpo(r2, r2),
                            pr_ok ? Dpr(r2, r2) : std::numeric_limits<double>::quiet_NaN(),
                            po_ok ? Dpo(r2, r2) : std::numeric_limits<double>::quiet_NaN()));
                    }
                }
            }
            // Card LGM: stash each phase-colour column's posterior (the estimate, its pure
            // conditional variance, its border regression) and its row of the station's
            // harvest block, so the next epoch folds it jointly with the ambiguities it
            // shares the phase rows with.  The coupling carry is the posture's (the
            // loader requires it); a column whose regression fails is not stashed and is
            // born again next epoch (counted).
            if (pc_in_cols && carry_cpl_cond) {
                const double t_week_pc = std::floor(pl_now_s / 604800.0);
                const double t_tow_pc  = pl_now_s - 604800.0 * t_week_pc;
                for (std::size_t k = 0; k < R.pl_pc.size(); ++k) {
                    const PlPc& pp = R.pl_pc[k];
                    if (pp.col < 0 || pp.col >= xr.size()) { ++ctx.counters.n_pc_stash_fail; continue; }
                    const double value_abs = pp.x_lin + xr(pp.col);
                    Eigen::VectorXd pcross;
                    auto hc = harvestCoupling(est_idx, pp.col, hblk ? &cols : nullptr,
                                              hblk ? &pcross : nullptr);
                    const double var = hc.first;
                    if (!std::isfinite(value_abs) || !(var > 0.0) || !std::isfinite(var)) {
                        ++ctx.counters.n_pc_stash_fail;
                        continue;
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(hc.second)};
                    const Eigen::Index prow = pc_row0 + static_cast<Eigen::Index>(k);
                    if (hblk && pcross.size() == hblk->C.rows() && prow < hblk->C.rows()) {
                        hblk->C.row(prow) = pcross.transpose();
                        ent.blk     = hblk;
                        ent.blk_idx = static_cast<int>(prow);
                    }
                    ctx.carry.pc_carry[{static_cast<int>(i), pp.sat}] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pp.col, SharedHistoryMap::Pc,
                                                  static_cast<int>(i), pp.sat);   // card SCL2 X2; no-op off
                    ++ctx.counters.n_pc_stash;
                    ctx.counters.pc_abs_sum += std::abs(value_abs);
                    ctx.counters.pc_sq_sum  += value_abs * value_abs;
                    if (ctx.knobs.pc_trace)
                        ctx.carry.pc_trace_rows.push_back(fmt::format(
                            "{:.0f},{:.1f},{},{},{:.2f},{:.6f},{:.6f},{}\n",
                            t_week_pc, t_tow_pc, stations[i].name, pp.sat, pp.elev_deg,
                            value_abs, std::sqrt(var), pp.carried ? 1 : 0));
                }
            }
        }
        // Stamp this harvest: the next epoch differences it against its own stamp for
        // the observed inter-epoch spacing.
        ctx.carry.pl_prev_now_s = pl_now_s;

        // Card LGM: prune phase-colour entries past the maximum gap.  Such an entry would
        // not be folded anyway (the inject re-births the line of sight); the prune keeps
        // the map to the live lines of sight and counts the edge.  The horizon is the
        // registry's (network.phase_colour_max_gap_s, the same double; card PRM phase 3b).
        if (ctx.knobs.pc_on)
            carryGapPrune(ctx.carry.pc_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::PhaseColour).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_pc_gap_prune;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::PcGapPrune);   // card PRM phase 3a
                          });

        // Carry-retirement census.  This prune is the mechanism behind the fleet-scale
        // carry loss that §1.3 calls the "cliff".  It reads only last_seen_s (it selects
        // on elapsed time and nothing else), so any claim that it improves quality has
        // to be measured against the quality-selected retirement paths, not assumed.
        // The fleet size is captured before the loop, so "fleet-scale" is a ratio.
        // The horizon is the registry's (network.phase_leveling_max_gap_s, the same double
        // as pl_max_gap_s; card PRM phase 3b).
        {
            const long long fleet_before = static_cast<long long>(local_carry.size());
            const long long n_pruned = carryGapPrune(
                local_carry, pl_now_s,
                ctx.params.spec(NetworkParamKind::Ambiguity).gap_horizon_s,
                [&](const auto& it) {
                    // Capture before the erase (RF-1 §7.4, D3).  This is the one
                    // retirement path that design §3.4 keeps as an erase ("this is a
                    // different arc"), so its inventory is the control against which
                    // the three that become bounded drops are read.
                    if (amb_state_on) {
                        const int sidx = std::get<0>(it->first);
                        ambStateNoteErase(
                            ctx,
                            static_cast<long long>(std::floor(pl_now_s / 604800.0)),
                            std::fmod(pl_now_s, 604800.0),
                            (sidx >= 0 && sidx < static_cast<int>(stations.size()))
                                ? stations[sidx].name : std::string{},
                            std::get<1>(it->first), std::get<2>(it->first),
                            it->second, "gap_prune");
                    }
                    // The decoupling edge of the carry itself (card SID phase 3, R3a).
                    // The key is buffered for the roster's audit pass, which owns the
                    // trace bundle and the epoch time and writes the
                    // `ARC arc_death reason=horizon` row (under its trace switch) and
                    // counts it; without that, this retirement would leave no record.
                    // The audit pass drains the vector every epoch.  A record only:
                    // nothing in the solve reads it.
                    if (ctx.roster.active)
                        ctx.carry.sid_carry_prune_deaths.push_back(it->first);
                    // Card PRM phase 3a: an arc the float vehicle's drop already dropped
                    // this epoch (networkAmbFloatApplyDrops stamps dropped_ep with this
                    // epoch's n_loop_epochs before PASS B; this harvest runs later in the
                    // same epoch, and the counter does not move in between).
                    if (it->second.dropped_ep > 0
                        && it->second.dropped_ep
                               == static_cast<long long>(ctx.counters.n_loop_epochs))
                        ++ctx.counters.prm_pruned_after_drop;
                });
            if (n_pruned > 0) {
                ctx.counters.n_cr_gap_prune += n_pruned;
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbGapPrune, n_pruned);   // card PRM phase 3a
                ++ctx.counters.n_cr_gap_prune_epochs;
                if (n_pruned > ctx.counters.n_cr_gap_prune_max) {
                    ctx.counters.n_cr_gap_prune_max       = n_pruned;
                    // Week/TOW, not pl_now_s: every trace CSV and log line in the run is
                    // keyed on TOW, so a continuous-GPS-seconds handle (~1.4e9) would not
                    // match anything a reader can search for.
                    ctx.counters.n_cr_gap_prune_max_week  =
                        static_cast<long long>(std::floor(pl_now_s / 604800.0));
                    ctx.counters.cr_gap_prune_max_tow     = std::fmod(pl_now_s, 604800.0);
                    ctx.counters.n_cr_gap_prune_max_fleet = fleet_before;
                }
            }
        }
        // --- In-block slant-iono carry (network.carry_iono_in_block): harvest each
        //     station's per-LOS iono posteriors through the same per-column solve the
        //     ambiguities used, write their rows of the station's harvest block, stash
        //     the absolute value, and prune the stale entries.
        //
        //     Independent loop (as for the ZWD), so it runs for stations whose
        //     ambiguity harvest produced nothing.  It differed from the harvest of the
        //     retired diagonal slant-iono carry (queue 8(b)) in three deliberate ways:
        //       * the cross terms are gathered (they subtract the B-fold over-count at
        //         inject); queue 8(b) took the diagonal alone;
        //       * the coupling row is harvested: an in-block column is folded like an
        //         ambiguity, border regression included, which makes the prior exact
        //         given the border (the rx-CB "no shared footprint" rationale applied to
        //         the diagonal vehicle, not to this one);
        //       * the value is absolute (x_lin + x_r).  network_ionosphere.csv writes
        //         x_r alone and is not frame-complete; a carry that is not
        //         frame-complete would re-inject a correction against a moved
        //         linearization point.
        //     A LOS whose harvest fails keeps blk = nullptr, so every pair with it is
        //     zeroed and counted (n_pl_blk_iono_missing_cross), which is the safe
        //     degrade.
        if (iono_blk_on) {
            auto& iono_blk_carry = ctx.carry.iono_blk_carry;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_iono.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> iblk = iblk_of_sta[i];
                const std::vector<Eigen::Index>* icross_cols =
                    iblk ? &iblk_cols_of_sta[i] : nullptr;
                const Eigen::Index irow0 = iblk ? iblk_row0_of_sta[i] : Eigen::Index{-1};
                std::vector<Eigen::Index> icols;
                icols.reserve(R.pl_iono.size());
                for (const auto& t : R.pl_iono) icols.push_back(std::get<1>(t));
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(est_idx, icols,
                                                                plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_iono.size(); ++a) {
                    const auto& [isat, icol, ix_lin] = R.pl_iono[a];
                    if (icol < 0 || icol >= xr.size()) continue;
                    const double value_abs = ix_lin + xr(icol);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    Eigen::VectorXd icross;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, icol, icross_cols,
                                                  iblk ? &icross : nullptr);
                        var = hc.first; coupling = std::move(hc.second);
                    } else {
                        var = (a < static_cast<std::size_t>(vard.size())) ? vard(a) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    const auto key = std::make_pair(static_cast<int>(i), isat);
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(coupling)};
                    // birth_ep / birth_s / var_first survive a re-stash and restart only
                    // on an erase-and-rebirth (a T4 erase, or a gap prune): the ambiguity
                    // carry's rule, applied through the ambiguity carry's own helper, so
                    // the age of an iono carry means the same thing.  (This map is not
                    // snapshotted, so it never sees a restored sentinel; the shared helper
                    // keeps the rule correct if it ever is.)
                    const auto oit = iono_blk_carry.find(key);
                    const LocalCarry* iprev =
                        (oit != iono_blk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = iprev ? iprev->var_first : var;
                    localCarryStampBirth(
                        ent, iprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row irow0 + a of the station's harvest block is this iono column's
                    // n_c read at every carried column of the station (its own entry
                    // equals `var`, the pure conditional).  The entry then points at the
                    // same PlHarvestBlock as the ambiguity and ZWD entries, so next
                    // epoch's inject pairs iono↔ambiguity, iono↔ZWD and iono↔iono by
                    // pointer equality, as it pairs ambiguity↔ambiguity.
                    if (iblk && irow0 >= 0 && icross.size() == iblk->C.rows()) {
                        const Eigen::Index irow = irow0 + static_cast<Eigen::Index>(a);
                        if (irow < iblk->C.rows()) {
                            iblk->C.row(irow) = icross.transpose();
                            ent.blk     = iblk;
                            ent.blk_idx = static_cast<int>(irow);
                        }
                    }
                    iono_blk_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, icol, SharedHistoryMap::Iono,
                                                  static_cast<int>(i), isat);   // card SCL2 X2; no-op off
                }
            }
            // Decoupling edge (invariants #1 and #7): a LOS unseen past the horizon is
            // dropped, not aged into a fold.  It is counted, because this is the
            // eclipse / set edge, and a run whose count is 0 over a full day did not
            // exercise it.  The horizon is the registry's: under the block carry it is
            // network.carry_iono_in_block_max_gap_s, the same double
            // (ctx.knobs.iono_blk_max_gap_s) this prune read before card PRM phase 3b.
            carryGapPrune(iono_blk_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::SlantIono).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_iono_blk_gap_drop;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::IonoGapPrune);   // card PRM phase 3a
                          });
        }
        // --- In-block receiver-clock carry (network.carry_rx_clock_in_block): harvest
        //     each station's receiver-clock (and ISB) posteriors through the same
        //     per-column solve the ambiguities used, write their rows of the station's
        //     harvest block, run the (bias, drift) filter, and prune the stale entries.
        //
        //     The drift is central to the design.  The producer never writes its
        //     solution back into the station StateManager, so x_lin(clock) is set once
        //     (0, or the latched cold_clock_seed median) and is constant for the run:
        //     x_r carries the entire clock, including a ramp of about 0.3 m/s, i.e. 9 m
        //     per 30 s epoch (0.29994 m/s on the sim truth bed, with 0.299 m random-walk
        //     increments about it).  A bias-only random walk at q = 0.09 m² would be
        //     about 30 σ wrong every epoch.  The carry is therefore the (bias, drift)
        //     pair: the block column holds the bias, and the drift is a per-station
        //     scalar Kalman state updated here from the harvested increments, the same
        //     state that supplies the one-step prediction whose innovation is the
        //     clock-jump detector.
        //
        //     Independent loop (as for the ZWD and iono), so it runs for stations whose
        //     ambiguity harvest produced nothing.  ISB columns share the loop but not
        //     the drift: an inter-system bias is a near-constant offset (the sim
        //     injects a fixed 8.000 m), so it takes a pure random walk.
        if (rxclk_blk_on) {
            auto& rx_clk_carry = ctx.carry.rx_clk_carry;
            auto& rx_clk_rate  = ctx.carry.rx_clk_rate;
            // The run's own observed harvest-to-harvest spacing, kept here rather than
            // read from ctx.carry.pl_prev_now_s, because that field has already been
            // advanced to pl_now_s when this loop runs.  Used only to convert a gap into
            // epochs for the per-epoch q, so the knob means the same thing at 1 s and at
            // 30 s.
            double dt_ep_run = (ctx.carry.rxclk_prev_now_s > 0.0)
                                   ? pl_now_s - ctx.carry.rxclk_prev_now_s : 0.0;
            if (!std::isfinite(dt_ep_run) || dt_ep_run <= 0.0 || dt_ep_run > rxclk_gap_s)
                dt_ep_run = 0.0;   // first epoch / implausible spacing ⇒ gap_ep = 1
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_rxclk.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> rblk = rblk_of_sta[i];
                const std::vector<Eigen::Index>* rcross_cols =
                    rblk ? &rblk_cols_of_sta[i] : nullptr;
                const Eigen::Index rrow0 = rblk ? rblk_row0_of_sta[i] : Eigen::Index{-1};
                std::vector<Eigen::Index> rcols;
                rcols.reserve(R.pl_rxclk.size());
                for (const auto& t : R.pl_rxclk) rcols.push_back(t.col);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(est_idx, rcols,
                                                                plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_rxclk.size(); ++a) {
                    const PlRxClk& pr = R.pl_rxclk[a];
                    if (pr.col < 0 || pr.col >= xr.size()) continue;
                    const double value_abs = pr.x_lin + xr(pr.col);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    Eigen::VectorXd rcross;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, pr.col, rcross_cols,
                                                  rblk ? &rcross : nullptr);
                        var = hc.first; coupling = std::move(hc.second);
                    } else {
                        var = (a < static_cast<std::size_t>(vard.size())) ? vard(a) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    const auto key = std::make_pair(static_cast<int>(i), pr.kind);
                    // --- (bias, drift) filter and the clock-jump response, for the primary
                    //     clock only (pr.kind empty); an ISB has no rate by construction.
                    if (pr.kind.empty()) {
                        auto& rt = rx_clk_rate[key];   // default-constructs on first sighting
                        if (rt.prev_s > 0.0) {
                            const double dt = pl_now_s - rt.prev_s;
                            if (dt > 0.0 && dt <= rxclk_gap_s) {
                                const double gap_ep = (dt_ep_run > 0.0)
                                                          ? std::max(1.0, dt / dt_ep_run) : 1.0;
                                // rate_var < 0 = "never updated": the drift is unknown, so
                                // the prediction is bias-only with an uninformative rate.
                                // This also keeps the jump gate correct on the first
                                // increment: with σ_d = 1 m/s the 30 s prediction band is
                                // about 30 m, which a 9 m/epoch drift passes at 0.3 σ and a
                                // 1 ms receiver step (3e5 m) fails by four orders of
                                // magnitude.
                                const double rvar_eff =
                                    (rt.rate_var >= 0.0) ? rt.rate_var : rxclk_rate_var0;
                                // The same kernel PASS B injected with, so the innovation
                                // is tested against the prediction that was used.
                                const RxClkPred rp = rxClockPredict(
                                    rt.prev_abs, rt.prev_var, rt.rate, rvar_eff, dt,
                                    gap_ep, rxclk_q);
                                const double var_pred = rp.var;
                                const double innov    = value_abs - rp.mean;
                                // Gate-scale census (invariant #4): the gate is
                                // k*sqrt(var_pred) metres and var_pred is per
                                // station-epoch, so a report can quote the size an event
                                // has to exceed rather than only k.  Measure-only.
                                if (var_pred > 0.0 && std::isfinite(innov)) {
                                    auto& C = ctx.counters;
                                    const double gate_m = rxclk_jump_k * std::sqrt(var_pred);
                                    if (C.n_rxclk_gate_tests == 0) {
                                        C.min_rxclk_gate_m = gate_m;
                                        C.max_rxclk_gate_m = gate_m;
                                    } else {
                                        C.min_rxclk_gate_m = std::min(C.min_rxclk_gate_m, gate_m);
                                        C.max_rxclk_gate_m = std::max(C.max_rxclk_gate_m, gate_m);
                                    }
                                    ++C.n_rxclk_gate_tests;
                                    C.sum_rxclk_gate_m += gate_m;
                                    C.max_rxclk_innov_m =
                                        std::max(C.max_rxclk_innov_m, std::abs(innov));
                                }
                                if (var_pred > 0.0 && std::isfinite(innov)
                                    && std::abs(innov) > rxclk_jump_k * std::sqrt(var_pred)) {
                                    // The receiver (or the solve) stepped (invariants #1
                                    // and #7).  Decoupling edge: erase both halves, so
                                    // nothing crosses the jump.  Re-coupling edge: the next
                                    // sighting is a cold rebirth whose rate prior is
                                    // uninformative again, so the two epochs after a jump
                                    // inject a nearly free prior rather than a stale one.
                                    const std::size_t prm_n_clk = rx_clk_carry.erase(key);
                                    rx_clk_rate.erase(key);
                                    ++ctx.counters.n_rxclk_carry_refused;
                                    // Card PRM phase 3a: the carried clock the jump erased.
                                    prmRespond(ctx.counters.prm_resp, ResponseSite::RxclkJumpErase,
                                               static_cast<long long>(prm_n_clk));
                                    continue;
                                }
                                // Scalar Kalman update of the drift from this increment.
                                // R_rate is the increment's own noise: two harvested
                                // variances plus the bias process noise, over Δt².
                                const double obs_rate = (value_abs - rt.prev_abs) / dt;
                                const double r_rate =
                                    (var + rt.prev_var + rxclk_q * gap_ep) / (dt * dt);
                                double rate_var_in = rvar_eff;
                                if (rxClockRateUpdate(rt.rate, rate_var_in, obs_rate,
                                                      r_rate, rxclk_rate_q * gap_ep)) {
                                    rt.rate_var = rate_var_in;
                                    ++rt.n_incr;
                                }
                            } else if (dt > rxclk_gap_s) {
                                // Past the horizon the drift extrapolation is not trusted
                                // either: reset the rate filter to its uninformative prior.
                                // The carry entry itself is erased by the prune below.
                                rt = {};
                            }
                        }
                        rt.prev_abs = value_abs;
                        rt.prev_s   = pl_now_s;
                        rt.prev_var = var;
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(coupling)};
                    // birth_ep / birth_s / var_first survive a re-stash and restart only
                    // on an erase-and-rebirth (a jump response, or a gap prune): the
                    // ambiguity carry's rule, applied through the ambiguity carry's own
                    // helper, so the age of a clock carry means the same thing.  (This map
                    // is not snapshotted either; see the note in the iono block above.)
                    const auto oit = rx_clk_carry.find(key);
                    const LocalCarry* rprev =
                        (oit != rx_clk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = rprev ? rprev->var_first : var;
                    localCarryStampBirth(
                        ent, rprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row rrow0 + a of the station's harvest block is this clock-family
                    // column's n_c read at every carried column of the station (its own
                    // entry equals `var`, the pure conditional).  The entry then points at
                    // the same PlHarvestBlock as the ambiguity / iono / ZWD entries, so
                    // next epoch's inject pairs clock↔ambiguity, clock↔iono and clock↔ZWD
                    // by pointer equality, as it pairs ambiguity↔ambiguity.
                    if (rblk && rrow0 >= 0 && rcross.size() == rblk->C.rows()) {
                        const Eigen::Index rrow = rrow0 + static_cast<Eigen::Index>(a);
                        if (rrow < rblk->C.rows()) {
                            rblk->C.row(rrow) = rcross.transpose();
                            ent.blk     = rblk;
                            ent.blk_idx = static_cast<int>(rrow);
                        }
                    }
                    rx_clk_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pr.col, SharedHistoryMap::RxClk,
                                                  static_cast<int>(i), pr.kind);   // card SCL2 X2; no-op off
                }
            }
            // Decoupling edge (invariants #1 and #7): a station unseen past the horizon
            // is dropped, not aged into a fold, and its drift state goes with it, because
            // extrapolating a rate across an unobserved hour is the stale-prior failure
            // the carry is designed to avoid.  Counted.  The horizon is the registry's
            // (network.carry_rx_clock_max_gap_s, the same double as rxclk_gap_s; card PRM
            // phase 3b).
            carryGapPrune(rx_clk_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::RxClock).gap_horizon_s,
                          [&](const auto& it) {
                              rx_clk_rate.erase(it->first);
                              ++ctx.counters.n_rxclk_carry_gap_drop;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::RxclkGapPrune);   // card PRM phase 3a
                          });
            ctx.carry.rxclk_prev_now_s = pl_now_s;
        }
        // --- RXCB-IN-BLOCK (card CTU section 14): harvest the receiver code biases -
        //     with their border regression and their row of the station's harvest
        //     block, so next epoch's inject folds them as coupling rows of the same
        //     whitened block as the ambiguities.  The value is the posterior absolute
        //     (x_lin + x_r), the variance the pure conditional (N_rr^-1)_cc, exactly as
        //     for every other block column.  The standalone diagonal harvest further
        //     down skips its stash under this knob (it still sums the produce counters
        //     and runs the gap prune).
        if (rxcb_blk_on) {
            auto& rx_cb_carry = ctx.carry.rx_cb_carry;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_cb.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> cblk = cblk_of_sta[i];
                const std::vector<Eigen::Index>* ccross_cols =
                    cblk ? &cblk_cols_of_sta[i] : nullptr;
                const Eigen::Index crow0 = cblk ? cblk_row0_of_sta[i] : Eigen::Index{-1};
                Eigen::VectorXd xr;
                try { xr = est.reconstructStation(est_idx, x_s); } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_cb.size(); ++a) {
                    const auto& [cb_key, ccol, cx_lin] = R.pl_cb[a];
                    if (ccol < 0 || ccol >= xr.size()) continue;
                    const double value_abs = cx_lin + xr(ccol);
                    if (!std::isfinite(value_abs)) continue;
                    Eigen::VectorXd ccross;
                    auto hc = harvestCoupling(est_idx, ccol, ccross_cols,
                                              cblk ? &ccross : nullptr);
                    const double var = hc.first;
                    if (!(var > 0.0) || !std::isfinite(var)) {
                        ++ctx.counters.n_rxcb_blk_harvest_fail;
                        continue;
                    }
                    const auto key = std::make_pair(static_cast<int>(i), cb_key);
                    // Card L5B: the satellite sum-to-zero of this receiver IFB's family
                    // re-seeded this epoch (no veteran satellite), so the satellite side
                    // realizes a new level of the family's null direction while a carried
                    // entry holds the old one.  Erase it (reset, not repair; counted) and
                    // skip this epoch's stash, whose posterior folded the old level: the
                    // next harvest re-seeds from a solve the stale entry no longer reaches.
                    // A column with no entry carried no old level and is stashed as usual
                    // (the cold start, where every channel seeds, is that case).
                    if (ctx.param_inventory.rx_ifb.estimated_by_knob
                        && rxIfbFamilyReseeded(stations[i].l5b, cb_key,
                                               ctx.carry.this_epoch_cb_reseed_channels)
                        && rx_cb_carry.erase(key) > 0) {
                        ++stations[i].l5b.reseed_erase;
                        continue;
                    }
                    // Event response (invariant #1): the step between this harvest's
                    // absolute and the carried one, against both variances, the random
                    // walk over the gap and the diagonal route's 0.5 m guard, at 10 sigma.
                    // Frame-invariant (two absolute realizations), unlike the inject
                    // screen it replaces.  A step erases the entry (reset, not repair):
                    // this epoch folded the stale prior, so its posterior is suspect too,
                    // and the next harvest re-seeds from a free solve.
                    {
                        const auto sit = rx_cb_carry.find(key);
                        if (sit != rx_cb_carry.end()) {
                            const double sgap = pl_now_s - sit->second.last_seen_s;
                            if (sgap >= 0.0 && sgap <= kRxCbMaxGapS) {
                                const double sv = var + sit->second.var
                                                  + ctx.knobs.rx_cb_q * sgap + 0.25;
                                if (std::abs(value_abs - sit->second.value_abs)
                                    > 10.0 * std::sqrt(sv)) {
                                    rx_cb_carry.erase(sit);
                                    ++ctx.counters.n_rxcb_blk_step_reset;
                                    prmRespond(ctx.counters.prm_resp, ResponseSite::RxcbStepErase);   // card PRM phase 3a
                                    continue;
                                }
                            }
                        }
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(hc.second)};
                    const auto oit = rx_cb_carry.find(key);
                    const LocalCarry* cprev = (oit != rx_cb_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = cprev ? cprev->var_first : var;
                    localCarryStampBirth(
                        ent, cprev, static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row crow0 + a of the station's harvest block is this column's n_c read
                    // at every carried column of the station, so next epoch's block pairs
                    // it with the ambiguities by pointer equality.
                    bool in_blk = false;
                    if (cblk && crow0 >= 0 && ccross.size() == cblk->C.rows()) {
                        const Eigen::Index crow = crow0 + static_cast<Eigen::Index>(a);
                        if (crow < cblk->C.rows()) {
                            cblk->C.row(crow) = ccross.transpose();
                            ent.blk     = cblk;
                            ent.blk_idx = static_cast<int>(crow);
                            in_blk = true;
                        }
                    }
                    if (!in_blk) ++ctx.counters.n_rxcb_blk_no_block;
                    rx_cb_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, ccol, SharedHistoryMap::RxCb,
                                                  static_cast<int>(i), cb_key);   // card SCL2 X2; no-op off
                    ++ctx.counters.n_rxcb_blk_harvested;
                    ++ctx.carry.rx_cb_harvested_total;
                }
            }
        }
        // --- ZWD carry (B9): harvest the posterior ZWD and stash it (mirrors the
        //     ambiguity harvest, in an independent loop so it works whether or not
        //     ambiguities are present).  Runs for B9 proper, for a live post-resume
        //     seam window (rolling B9), and, capture-only with nothing fed back, on any
        //     run with a snapshot path, so every RTB-7 snapshot serializes fresh Z rows
        //     (§17 finding 3).  The capture path is read-only with respect to the
        //     solve: R.pl_zwd_present is record-only on those epochs and no inject
        //     fires.
        if (zwd_on || zwd_capture || zwd_seam_live) {
            for (size_t i = 0; i < stations.size(); ++i) {
                // Harvest the pivot's ZWD too (B9): R.pl_zwd_present is set for the pivot
                // because the carry inject also runs for it.
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (!R.pl_zwd_present) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(
                            est_idx, {R.pl_zwd.col}, plan.shared_datum);
                } catch (...) { continue; }
                if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
                const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): when
                // this station's pl harvest above put the ZWD column in `cols` (and only
                // then), the ZWD is harvested through the same cross-gather every
                // ambiguity column used (one call, one solve), and its row is written at
                // block index n = cols.size()−1.  zblk is null on every other
                // station-epoch, and off ⇒ nullptr/nullptr, the call as it is without the
                // knob (byte-identical).
                const std::shared_ptr<PlHarvestBlock> zblk =
                    zwd_blk_on ? zblk_of_sta[i] : nullptr;
                const std::vector<Eigen::Index>* zcross_cols =
                    zblk ? &zblk_cols_of_sta[i] : nullptr;
                Eigen::VectorXd zcross;
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, R.pl_zwd.col, zcross_cols,
                                              zblk ? &zcross : nullptr);
                    var = hc.first; coupling = std::move(hc.second);
                } else {
                    var = (vard.size() > 0) ? vard(0) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                // Prior-residual census hook for the ZWD (INC-2): the ZWD carry has no
                // screen of any kind (no slip detector, no jump test, no D5 analogue),
                // which is the standing-invariant-#1 gap the census inventories.  Unlike
                // pl, `folded` is recorded directly and needs no reproduction: PASS B
                // records R.pl_zwd_injected at the injector.  The prior is read before
                // the stash below overwrites it.  Also gated on an inject-capable epoch
                // (B9 or a live seam window), so the capture-only path adds no rows and
                // the census of a run with a snapshot path stays byte-identical to one
                // without the capture.
                if (prires_zwd && prires_est.enabled() && (zwd_on || zwd_seam_live)) {
                    const auto zit = zwd_carry.find(static_cast<int>(i));
                    if (zit != zwd_carry.end()) {
                        const double zgap = pl_now_s - zit->second.last_seen_s;
                        if (zgap >= 0.0 && zgap <= pl_max_gap_s) {
                            NetworkPriorResidual::Input in;
                            in.family   = "zwd";
                            in.station  = static_cast<int>(i);
                            in.e        = zit->second.value_abs - value_abs;
                            // Mirror the injector's q exactly (RS-10b): inside a live
                            // seam window an armed network.zwd_seam_reseed_q applies
                            // there, and a census computed against a variance no row used
                            // would report the `zwd` family's σ̂₀ (the R1' instrument) off
                            // by the override ratio.
                            in.fold_var = zit->second.var
                                        + ctx.knobs.zwdQEff(zwd_seam_live)
                                              * std::max(0.0, zgap);
                            in.post_var = var;
                            in.gap_s    = zgap;
                            in.folded   = R.pl_zwd_injected;
                            // A ZWD carry within its horizon that did not inject is not
                            // a mechanism response (nothing fired on it); the injector had
                            // no live column or a non-positive var_eff.  CLEAN with the
                            // `+` branch is the correct classification, and the mech tag
                            // records it.
                            in.label = PriorResidualLabel::Clean;
                            in.mech  = R.pl_zwd_injected ? "" : "not_injected";
                            prires_est.addRow(in);
                        }
                    }
                }
                // Observatory, ZWD half (§18.12).  The Z rows are restored and counted
                // like the L rows (n_resume_cpl_zrows, and they arm the same
                // resume_cpl_census_pending latch), so a census that covered only the L
                // side would report rel ≈ 0 on a leg whose ZWD regression was stale, an
                // instrument that reads clean because it never looked (invariant #4).
                // Same measure-only body as the phase-leveling census above; read before
                // the stash below moves `coupling` out.
                if (ctx.carry.resume_cpl_census_pending && carry_cpl_cond
                    && !coupling.empty()) {
                    const auto zold = zwd_carry.find(static_cast<int>(i));
                    if (zold != zwd_carry.end() && !zold->second.coupling.empty()) {
                        const auto idOfZ = [](const CoupledShared& c) {
                            return std::make_tuple(static_cast<int>(c.kind), c.sat,
                                                   c.signal, c.n, c.m,
                                                   static_cast<int>(c.trig), c.sh_layer);
                        };
                        std::map<decltype(idOfZ(coupling.front())), double> znow;
                        double zmaxabs = 0.0, zrhs_new = 0.0, zrhs_old = 0.0, zdmax = 0.0;
                        for (const CoupledShared& cs : coupling) {
                            znow[idOfZ(cs)] = cs.coeff;
                            zmaxabs  = std::max(zmaxabs, std::abs(cs.coeff));
                            zrhs_new += cs.coeff * cs.s_prev;
                        }
                        long long zmatched = 0, zgone = 0;
                        for (const CoupledShared& cs : zold->second.coupling) {
                            zrhs_old += cs.coeff * cs.s_prev;
                            const auto h = znow.find(idOfZ(cs));
                            if (h == znow.end()) { ++zgone; continue; }
                            ++zmatched;
                            zdmax = std::max(zdmax, std::abs(cs.coeff - h->second));
                        }
                        ++ctx.counters.n_resume_cpl_census_rows;
                        ctx.counters.n_resume_cpl_census_terms     += zmatched;
                        ctx.counters.n_resume_cpl_census_unmatched += zgone;
                        if (zmaxabs > 0.0)
                            ctx.carry.resume_cpl_census_rel.push_back(zdmax / zmaxabs);
                        ctx.carry.resume_cpl_census_rhs.push_back(
                            std::abs(zrhs_new - zrhs_old));
                    }
                }
                zwd_carry[static_cast<int>(i)] =
                    LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
                networkSharedHistoryNoteEntry(shx_h, est_idx, R.pl_zwd.col, SharedHistoryMap::Zwd,
                                              static_cast<int>(i));   // card SCL2 X2; no-op off
                // ZWD in the local block: row n of the station's harvest block is the
                // ZWD column's own n_c read at every carried column (its last entry
                // equals `var`, the pure conditional).  The entry then points at the same
                // PlHarvestBlock as the ambiguity entries, so next epoch's inject pairs
                // ZWD↔ambiguity by pointer equality, as it pairs ambiguity↔ambiguity,
                // and with nothing stashed at a different harvest.  A ZWD whose harvest
                // failed above does not reach here: its entry keeps blk = nullptr, and
                // every pair with it is zeroed and counted (pl_blk_zwd_missing_cross),
                // which is the safe degrade.
                if (zblk && zcross.size() == zblk->C.rows()) {
                    const Eigen::Index zrow = zblk->C.rows() - 1;   // the appended column
                    zblk->C.row(zrow) = zcross.transpose();
                    auto& zent = zwd_carry[static_cast<int>(i)];
                    zent.blk     = zblk;
                    zent.blk_idx = static_cast<int>(zrow);
                }
            }
            // The horizon is the registry's (network.phase_leveling_max_gap_s, the same
            // double as pl_max_gap_s; card PRM phase 3b).
            carryGapPrune(zwd_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::Zwd).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_cr_zwd_gap_prune;   // same predicate, same census
                              prmRespond(ctx.counters.prm_resp, ResponseSite::ZwdGapPrune);   // card PRM phase 3a
                          });
            // Seam-window bookkeeping (network.zwd_seam_reseed): count this epoch's
            // actual folds (R.pl_zwd_injected is the injector's own record), advance the
            // retirement clock, and log the decoupling edge when the horizon is
            // exhausted (invariant #7; the ARMED line at the resume was the re-coupling
            // edge).  Serial section: the parallel produce phase only reads
            // zwd_seam_ep_left.
            if (zwd_seam_live) {
                long long seam_folds = 0;
                for (size_t i = 0; i < stations.size(); ++i)
                    if (est_index_of[i] >= 0 && asm_res[i].pl_zwd_injected) ++seam_folds;
                ctx.counters.n_zwd_seam_folds += seam_folds;
                if (seam_folds > 0) ++ctx.counters.n_zwd_seam_epochs;
                if (--ctx.carry.zwd_seam_ep_left == 0) {
                    // This seam has no logger parameter: resolve the logger per call and
                    // null-guard it (the spdlog free-function / cached-static hazard: do
                    // not cache it, do not dereference it unguarded).
                    if (auto lg = spdlog::get("main_log"))
                        lg->info("Network (ZWD seam re-seed): RETIRED — the {}-epoch "
                                 "post-resume window is exhausted at tow {:.1f}; {} "
                                 "station-fold(s) over {} epoch(s) rode the window.  The "
                                 "station filters carry their own ZWD memory from here; "
                                 "continuous epochs never carry.",
                                 ctx.knobs.zwd_seam_epochs,
                                 std::fmod(pl_now_s, 604800.0),
                                 ctx.counters.n_zwd_seam_folds,
                                 ctx.counters.n_zwd_seam_epochs);
                }
            }
        }
        // Card SCL: every carried column of every station is harvested; add the clones'
        // within-station conditional covariance to the F3 blocks.  No-op off.
        networkClockCloneHarvestEnd(ccl_h, ctx);
        // Card SCL2 X2: with every carried row of the network harvested, compress the rows'
        // shared conditional term into the history and add each row's remainder (B1's end
        // above returns at once under X2).  No-op off.
        networkSharedHistoryHarvestEnd(shx_h, ccl_h, ctx, pl_now_s);
        // --- RPB carry (card 3, eq34): harvest δ̂_{r,σ}, stash and prune ----------
        // Mirrors the ZWD harvest.  There is no card-1 coupling harvest for RPB
        // columns, on purpose: coupling rows would give the RPB a de facto
        // shared-border footprint (the D6 v2 caveat zone, tuGaugeRestore group
        // exclusions); the diagonal conditional variance keeps the carry strictly
        // local and conservative.
        if (eq34_rpb_q > 0.0) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_rpb.empty()) continue;
                const std::size_t est_idx =
                    static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                std::vector<Eigen::Index> cols;
                cols.reserve(R.pl_rpb.size());
                for (const auto& t : R.pl_rpb) cols.push_back(std::get<1>(t));
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        est_idx, cols, plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_rpb.size(); ++a) {
                    const auto& [rpb_key, rcol, rx_lin] = R.pl_rpb[a];
                    if (rcol < 0 || rcol >= xr.size()) continue;
                    const double value_abs = rx_lin + xr(rcol);
                    const double var = (a < static_cast<std::size_t>(vard.size()))
                                           ? vard(a) : -1.0;
                    if (!std::isfinite(value_abs) || !(var > 0.0)
                        || !std::isfinite(var)) continue;
                    rpb_carry[{static_cast<int>(i), rpb_key}] =
                        LocalCarry{value_abs, var, pl_now_s};
                }
            }
            // The horizon is the registry's (network.phase_leveling_max_gap_s, the same
            // double as pl_max_gap_s; card PRM phase 3b).
            carryGapPrune(rpb_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::RxPhaseBias).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_cr_rpb_gap_prune;   // same predicate, same census
                              prmRespond(ctx.counters.prm_resp, ResponseSite::RpbGapPrune);   // card PRM phase 3a
                          });
        }
    }

    // --- Receiver-DCB datum gauge census (RXCB-DATUM) --------------------------
    // The zero-mean receiver-DCB datum's fold count, in its own walk, gated on its own
    // knob.  network.rx_code_bias_datum and network.carry_rx_code_bias are independent,
    // and the datum without the carry is the canonical posture; counting this inside
    // the carry bracket below would leave n_rxcb_gauges at 0 on every such run, and the
    // run-end RXCB-DATUM line would then raise its invariant #4 alarm against a working
    // gauge.
    //
    // This is the only accumulation site, so a run with both armed cannot double count.
    // It is also byte-identical on the carry-only posture: R.pl_cb_datum_rows is
    // incremented only under `rx_cb_datum` (the RXCB-DATUM row build in
    // ApplicationNetworkAssembly.cpp), so it is 0 whenever this walk is skipped.
    if (ctx.knobs.rx_cb_datum) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            ctx.counters.n_rxcb_gauges +=
                static_cast<size_t>(asm_res[i].pl_cb_datum_rows);
        }
    }

    // --- rx-CB carry: harvest, stash and prune -------------------------------
    // Outside the carry_warm bracket above on purpose: the receiver code-bias carry has
    // to be live on the carry_invariant lineage without phase leveling (the carry_warm
    // gate is what makes the ZWD carry inert there).  Mirrors the RPB harvest: several
    // columns per station, conditional local variance, LocalCarry stash keyed
    // (station, CB key).  The produce phase only reads the map (find, never insert or
    // erase) and counts its decisions in R, which are summed here serially, so the whole
    // cycle is race-free by construction.
    if (ctx.knobs.rx_cb_on) {
        auto& rx_cb_carry = ctx.carry.rx_cb_carry;
        // The horizon is kRxCbMaxGapS (filter/NetworkRunTypes.hpp; spec §3: housekeeping
        // horizon), the one definition the inject, this harvest and the registry read.
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            ctx.carry.rx_cb_injected_total += R.pl_cb_injected;
            // n_rxcb_gauges is not summed here: it belongs to the RXCB-DATUM knob, not
            // the carry, and has its own walk above.  Every counter left in this bracket
            // is carry-only: their producer-side increments all sit below the
            // `if (!rx_cb_on) continue;` in the assembly's pl_cb walk, so a datum-only run
            // cannot populate them.
            ctx.carry.rx_cb_innov_skips_total += R.pl_cb_innov_skips;
            // The same two tallies, kept per station (RS-9 rider, §21.18).  The pooled
            // "N folded, M withheld" line cannot tell a share of stations refused
            // occasionally from a similar share refused always; in the second case the set
            // is fixed from k = 1, and those stations carry no receiver-DCB memory for the
            // whole run while the line still counts them among the "N carried".  The
            // denominator is station-epochs with a decision; the numerator is
            // station-epochs in which every decision was a withhold ("carried nothing",
            // the multi-family generalization of the one-key field posture).  Serial, so
            // no race.
            if (R.pl_cb_injected > 0 || R.pl_cb_innov_skips > 0) {
                auto& tally = ctx.carry.rx_cb_sta_tally[stations[i].name];
                ++tally.first;
                if (R.pl_cb_injected == 0) ++tally.second;
            }
            // RXCB-IN-BLOCK: the stash happened in the block harvest above (with the
            // border regression), and only while the leveling carry is warm; a cold
            // entry must not be written here without one.
            if (ctx.knobs.rxcb_blk) continue;
            if (R.pl_cb.empty()) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            Eigen::VectorXd xr, vard;
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_cb.size());
            for (const auto& t : R.pl_cb) cols.push_back(std::get<1>(t));
            try {
                xr = est.reconstructStation(est_idx, x_s);
                vard = est.conditionalLocalVarianceDiag(
                    est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < R.pl_cb.size(); ++a) {
                const auto& [cb_key, ccol, cx_lin] = R.pl_cb[a];
                if (ccol < 0 || ccol >= xr.size()) continue;
                const double value_abs = cx_lin + xr(ccol);
                const double var = (a < static_cast<std::size_t>(vard.size()))
                                       ? vard(a) : -1.0;
                if (!std::isfinite(value_abs) || !(var > 0.0)
                    || !std::isfinite(var)) continue;
                // Card L5B: a re-seeded family's carried receiver IFB entries are erased and
                // not re-stashed this epoch, as in the block harvest above (reset, not
                // repair; counted).
                if (ctx.param_inventory.rx_ifb.estimated_by_knob
                    && rxIfbFamilyReseeded(stations[i].l5b, cb_key,
                                           ctx.carry.this_epoch_cb_reseed_channels)
                    && rx_cb_carry.erase({static_cast<int>(i), cb_key}) > 0) {
                    ++stations[i].l5b.reseed_erase;
                    continue;
                }
                // Prior-residual census hook (INC-2).  `folded` is re-derived by calling
                // rxCbInjectGate with the same arguments PASS B used: the gate is a pure
                // function, and every input is still in hand and unchanged at this point
                // (the stash below overwrites the entry), so this reproduces the
                // injector's own decision exactly rather than mirroring it.  The
                // SkipInnov branch is an invariant #1 event response (receiver reset or
                // firmware bias jump: withhold the fold, let the harvest re-seed), so it
                // is the RESPONDED positive control for this family.
                if (prires_rxcb && prires_est.enabled()) {
                    const auto pit = rx_cb_carry.find({static_cast<int>(i), cb_key});
                    if (pit != rx_cb_carry.end()) {
                        const double cgap = pl_now_s - pit->second.last_seen_s;
                        const double cinnov = pit->second.value_abs - cx_lin;
                        double cvar_eff = 0.0;
                        const RxCbInjectAction cact = rxCbInjectGate(
                            cgap, kRxCbMaxGapS, pit->second.var, ctx.knobs.rx_cb_q,
                            cinnov, /*k_sigma=*/10.0, /*guard_var=*/0.25, cvar_eff);
                        // SkipGap leaves cvar_eff at 0 (the gate returns before setting
                        // it), so there is no fold variance to standardize against: fall
                        // back to the nominal recompute and mark the row as raw formal
                        // (var_nominal) rather than invent a scale.
                        const bool have_var = cvar_eff > 0.0;
                        NetworkPriorResidual::Input in;
                        in.family   = "rxcb";
                        in.signal   = cb_key;
                        in.station  = static_cast<int>(i);
                        in.e        = pit->second.value_abs - value_abs;
                        in.fold_var = have_var
                            ? cvar_eff
                            : pit->second.var
                                + ctx.knobs.rx_cb_q * std::max(0.0, cgap);
                        in.post_var = var;
                        in.gap_s    = std::max(0.0, cgap);
                        in.folded   = (cact == RxCbInjectAction::Fold);
                        in.var_nominal = !have_var;
                        in.label = (cact == RxCbInjectAction::SkipInnov)
                                       ? PriorResidualLabel::Responded
                                       : PriorResidualLabel::Clean;
                        in.mech  = (cact == RxCbInjectAction::SkipInnov) ? "innov_withhold"
                                 : (cact == RxCbInjectAction::SkipGap)   ? "gap" : "";
                        prires_est.addRow(in);
                    }
                }
                rx_cb_carry[{static_cast<int>(i), cb_key}] =
                    LocalCarry{value_abs, var, pl_now_s};
                ++ctx.carry.rx_cb_harvested_total;
            }
        }
        // The horizon is the registry's (kRxCbMaxGapS, the same constant; card PRM phase 3b).
        carryGapPrune(rx_cb_carry, pl_now_s,
                      ctx.params.spec(NetworkParamKind::RxCodeBias).gap_horizon_s,
                      [&](const auto&) {
                          prmRespond(ctx.counters.prm_resp, ResponseSite::RxcbGapPrune);   // card PRM phase 3a
                      });
    }

    // Retire the first-warm-epoch latch at the end of this epoch's serial harvest
    // (§18.12).  Everything the latch gates (the C1 diagonal-border count in
    // networkInjectD2CarryPrior and the present/departed fold split, both earlier in the
    // epoch, and the restored-against-fresh regressor census above) therefore covers
    // exactly the first
    // post-resume epoch.  Unconditional: a run that did not resume leaves it false, and
    // clearing a false bool is a no-op on every off path.
    if (ctx.carry.resume_cpl_census_pending)
        // Second instrument of P9a (RS-7, §21.14).  The warm resume runs before any
        // epoch, so every Q3 decouple counted by now belongs to the first post-resume
        // epoch, with no baseline subtraction and no per-epoch plumbing.  When the
        // seam's carried rows meet a border with no cross-covariance, many anchored
        // clock priors are out-voted at the first fold (§18.11); with the border
        // restored, this count is expected to match the continuous run's level.
        ctx.counters.n_resume_first_fold_cta_trips =
            ctx.counters.cta_trip_nis + ctx.counters.cta_trip_abs
          + ctx.counters.cta_trip_gap + ctx.counters.cta_trip_datum
          + ctx.counters.cta_trip_ext;
    ctx.carry.resume_cpl_census_pending = false;
}

} // namespace GnssProcessor
