/**
 * @file ZwdCarry.cpp
 * @brief Zenith wet delay (ZWD) carry: the between-epoch prior on each station's wet delay and the unconditional record of its ZWD column for snapshots.
 *
 * Each station has one ZWD state. When the carry (network.carry_zwd), the post-resume seam
 * window (network.zwd_seam_reseed) or snapshot capture is active, the harvest stores each
 * station's posterior after the solve in net::StationState::zwd (value, variance, time of the
 * stash and, with network.carry_conditional_coupling, its regression on the shared border).
 * When the carry or the seam window is active, the next epoch's station build turns that entry
 * into a prior on the ZWD column, with the variance grown by a random walk over the time since
 * the stash. Capture alone only records the posterior for snapshots and feeds nothing back.
 *
 * The file has three parts:
 * - Station build steps (networkZwdRowCapture, networkZwdCarryPrior). They run in the parallel
 *   per-station produce of PASS B (networkPassB_) and write only their own station's slot.
 * - The harvest (networkHarvestZwd, networkZwdSeamWindowStep). It runs serially after the solve and
 *   is called by networkHarvestLocalCarries (network/carry/LocalCarryHarvest.hpp).
 * - The run-end report (networkZwdSeamRunEnd).
 */

#include "network/carry/ZwdCarry.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/assembly/StationBuild.hpp"   // StationBuild, StationCarryFrame
#include "network/driver/Epoch.hpp"
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/carry/Coupling.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/state/StationParams.hpp"   // the station's parameter set
#include "rinex/MultiRinexSync.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "rinex/MultiRinexSync.hpp"
#include "network/solve/Estimator.hpp"
#include "filter/IlsCore.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/assembly/ExtraBandScope.hpp"  // the extra-band / primary-pair predicates
#include "network/assembly/SecondCodeScope.hpp" // secondCodeCellEdges, the two cell edges
#include "network/assembly/SecondPhaseScope.hpp" // the cell edges + the witness
#include "network/solve/PivotHandover.hpp"   // pivot-handover datum-change detector
#include "network/carry/CarryVce.hpp"        // least-squares variance-component monitor of the carry rows
#include "network/carry/InvariantCarryDriver.hpp"  // double-difference (DD) cycle-basis invariant carry
#include "network/ar/SdEwlAr.hpp"               // the shared extra-wide-lane band table (sdEwlBands)
#include "network/carry/Coupling.hpp"       // conditional coupling-row builder
#include "network/assembly/ParamInventory.hpp" // the parameter inventory and the receiver IFB rule
#include "network/assembly/Contributions.hpp" // row tags and the contribution tallies
#include "network/qc/PrefitCensus.hpp"   // prefit census
#include "network/diag/ShDevVtec.hpp"     // networkShDevCarryAge
#include "network/assembly/StationBuild.hpp"  // networkAssemblyTally
#include "network/solve/SolveStations.hpp" // the consume and the in-solve track
#include "network/solve/ChainDatum.hpp"    // networkEq34PinAudit
#include "network/ar/ArSuspect.hpp"      // arSuspectNote: the AR search's suspect-corner stamps
#include "network/driver/RunTypes.hpp"      // run-loop record types: NetStation / StationAssembly / carry PODs
#include "network/qc/RobustStats.hpp"   // median / MAD (median absolute deviation) robust attribution helpers
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs
#include "network/diag/RunTraces.hpp" // trace-stream ownership + open logic + roster + FlagRate
#include "network/state/RunContext.hpp"    // ctx: knobs, carry state, service counters
#include "network/driver/SetupResult.hpp"   // setup-phase handoff (setup) + navAvailabilityGpsSeconds
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair
#include "slip/TurboEditResolver.hpp"   // per-signal Melbourne-Wubbena (MW) and geometry-free (GF) integer repair
#include "slip/IfPhaseSlipDetector.hpp" // ionosphere-free (IF) phase slip detection between satellites and between epochs
#include "network/slip/SlipQc.hpp"       // slip-QC pure helpers: ifElevMap / parseIfPairResetKey
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // identity reads (systemOfSat, ObservableId, ...), carriers
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to one thread inside the per-station OpenMP region (no nested oversubscription, deterministic result)
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/carry/LocalCarryHarvest.hpp"   // HarvestBorder, StationHarvestBlock
#include "network/diag/AmbStateTrace.hpp"   // AmbStateRow / ambStateNoteErase
#include "network/ar/AmbFloat.hpp"       // ambFloatBirthFloor
#include "network/qc/PriorResidual.hpp"    // prior-residual census
#include "network/solve/NormalEquations.hpp"
#include <spdlog/spdlog.h>

namespace GnssProcessor {

// ---- Station build steps (PASS B) ----

StationStep networkZwdRowCapture(StationBuild& build) {
    // ---- From the station build ----
    const NetworkRunContext& ctx = build.ctx;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    // ---- End of the bindings ----

    // Record the ZWD column and its linearization point for the post-solve harvest
    // on any run that writes snapshots (zwd_capture), so every snapshot serializes
    // fresh ZWD rows even without the carry. Record only: there is no carry lookup
    // and no fold, and the state vector is read as a copy, so the solve is unchanged.
    // Skipped when the carry step (networkZwdCarryPrior) already recorded the same
    // fields (R.pl_zwd_present). It sits outside that step's legacy_local_inject
    // gate on purpose, so the capture also works during the phase-leveling warmup
    // window and on runs without phase leveling.
    if (!(zwd_capture && !R.pl_zwd_present)) return StationStep::Next;
    const int tropo_idx = st.params.zwd;   // -1: the station has no wet-delay column
    if (tropo_idx >= 0) {
        const Eigen::VectorXd& x_lin_z = build.postUpdateState();
        if (tropo_idx < x_lin_z.size() && std::isfinite(x_lin_z(tropo_idx))) {
            const auto zhit = h2local.find(tropo_idx);
            if (zhit != h2local.end()) {
                R.pl_zwd_present = true;
                R.pl_zwd = PlZwd{static_cast<Eigen::Index>(zhit->second),
                                 x_lin_z(tropo_idx)};
            }
        }
    }
    return StationStep::Next;
}

void networkZwdCarryPrior(StationCarryFrame& carry) {
    // ---- From the station build ----
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    double pl_now_s = build.clock.pl_now_s;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const Eigen::VectorXd& x_lin = carry.x_lin;
    CouplingRowBuilder& cb = carry.cb;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // conditional local carry on coupling rows; requires fullcov_on (enforced at load)
    const bool   zwd_on        = ctx.knobs.zwd_on;                 // carry each station's ZWD across epochs (network.carry_zwd)
    // Seam window: the bounded run of epochs after a warm resume
    // (network.zwd_seam_reseed) in which the restored ZWD entries are folded like
    // the continuous carry. zwd_seam_live is true while it is open. It is stable
    // across the parallel produce phase, because the countdown is decremented
    // serially at the post-solve harvest.
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    // Random-walk process noise q (m^2/s) for this epoch's ZWD folds. It is
    // network.zwd_carry_q, except while the seam window is open and
    // network.zwd_seam_reseed_q is set, which replaces it. The seam has its own q
    // because zwd_carry_q is in the warm-resume fingerprint (a changed continuous
    // q must cold-start) while the seam response is not, so the seam's stiffness
    // could not otherwise be tuned on a resume leg. Every ZWD fold reads this
    // value, never zwd_q directly, and it is resolved once here from the same
    // zwd_seam_live the fold gates read. The diagonal row, the in-block column
    // and the harvest's prior-residual census therefore agree on the q folded.
    const double zwd_q_eff     = ctx.knobs.zwdQEff(zwd_seam_live);
    // In-block route (network.carry_zwd_in_block): the carried ZWD column joins the
    // station's whitened local block instead of folding as a separate diagonal
    // row, which would repeat a station-common direction that every carried
    // ambiguity row already asserts. Conjoined with pl_blk_on, so a station-epoch
    // on the per-column route is never touched and keeps the diagonal ZWD row.
    // The loader disables the knob (with a warning) unless carry_zwd and
    // phase_leveling_local_block are both on; this is the live half of that gate.
    const bool   zwd_blk_on    = ctx.knobs.zwd_blk_on;
    const net::StationTable& sta_rows = ctx.entities.stations;   // read-only: the parallel produce
    // ---- End of the bindings ----

    // Carry the ZWD column as one more local state. It is a random walk (positive
    // q), not a random constant. The prior tuple joins `inj`, so it is sorted with
    // the other priors and folded by the single addStationPrior inject. While the
    // seam window is open (zwd_seam_live) the same fold runs: the first epoch
    // folds the restored ZWD posterior, later epochs the rolling re-stash.
    if (!(legacy_local_inject && (zwd_on || zwd_seam_live))) return;
    const int tropo_idx = st.params.zwd;   // -1: the station has no wet-delay column
    if (tropo_idx >= 0) {
        if (tropo_idx < x_lin.size()) {
            const auto zhit = h2local.find(tropo_idx);
            if (zhit != h2local.end() && std::isfinite(x_lin(tropo_idx))) {
                const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                R.pl_zwd_present = true;   // recorded for the post-solve stash
                R.pl_zwd = PlZwd{zcol, x_lin(tropo_idx)};
                const auto* zit = net::get(sta_rows, StationId{static_cast<int>(i)},
                                           &net::StationState::zwd);
                // Skip the diagonal row when this station-epoch's ZWD has already
                // folded as a column of the local block: both would assert the
                // same direction, the double count the in-block route removes.
                if (zit != nullptr && !R.pl_zwd_in_block) {
                    const double zgap = pl_now_s - zit->last_seen_s;
                    if (zgap <= pl_max_gap_s) {
                        const double zvar_eff =
                            zit->var
                            + zwd_q_eff * std::max(0.0, zgap);
                        if (zvar_eff > 0.0) {
                            if (carry_cpl_cond) {
                                cb.buildCplRow(zcol, x_lin(tropo_idx),
                                            *zit, zvar_eff,
                                            std::max(0.0, zgap),
                                            SatId{}, SignalId{});
                                R.pl_zwd_injected = true;   // a carry prior was actually folded
                            } else {
                                const double zlambda = 1.0 / zvar_eff;
                                const double zeta =
                                    zlambda * (zit->value_abs - x_lin(tropo_idx));
                                if (std::isfinite(zeta)) {
                                    inj.emplace_back(zcol, zlambda, zeta,
                                                     RowTag{ContributionKind::PriorZwdCarry, {}, {}});
                                    R.pl_zwd_injected = true;   // a carry prior was actually folded
                                }
                            }
                            // A diagonal ZWD row emitted while the in-block route is
                            // armed is a fallback. Either the station had no block
                            // this epoch (no carried ambiguities, or the carry is
                            // not warm for the ambiguity half), or the block's
                            // factorization failed after the ridge retry
                            // (n_pl_blk_fallback attributes which). The behaviour
                            // falls back to the plain diagonal carry, never to
                            // "no ZWD memory", but it is not the route under test,
                            // so it is counted.
                            if (zwd_blk_on && R.pl_zwd_injected)
                                ++R.pl_blk_zwd_fallback_diag;
                        }
                    } else if (carryPastHorizon(pl_now_s, zit->last_seen_s,
                                                pl_max_gap_s)) {
                        // A carried wet delay past the gap horizon is not folded.
                        // It is counted here once for both routes: an in-block gap
                        // refusal leaves pl_zwd_in_block false, so it reaches this
                        // gate with the same entry and the same test.
                        ++R.prm_zwd_gap_skip;
                    }
                }
            }
        }
    }
}

// ---- End of the station build steps ----

// ---- Harvest ----

void networkHarvestZwd(NetworkRun& run, const NetworkEpoch& ep, HarvestBorder& border,
                       const std::vector<StationHarvestBlock>& blocks) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const Eigen::VectorXd& x_s = ep.solve.x_s;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const NetworkSbasis::Plan& plan = ep.solve.plan;
    NetworkPriorResidual& prires_est = run.engines.prires;
    double pl_now_s = ep.clock.pl_now_s;
    net::StationTable& sta_rows = ctx.entities.stations;   // the station carries
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const bool   zwd_on = ctx.knobs.zwd_on;
    const bool   prires_zwd  = ctx.knobs.prires_zwd;
    // In-block route (network.carry_zwd_in_block): the ZWD column joins the
    // station's local harvest block. It uses the same live predicate as the block
    // itself (pl_local_block && carry_cpl_cond), so the harvest and the inject agree
    // on which station-epochs have a block. When off, `cols` is untouched and
    // PlHarvestBlock::C has no ZWD column.
    const bool   zwd_blk_on  = ctx.knobs.zwd_blk_on;
    // Capture: a run that writes snapshots records the ZWD posterior every epoch,
    // so every snapshot carries ZWD rows.
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    // Seam window (network.zwd_seam_reseed): open for a bounded run of epochs after
    // a warm resume.
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    SharedHistoryHarvest& shx_h = border.shx_h;
    // ---- End of the bindings ----
    // Harvest the posterior ZWD and stash it. This is its own loop, beside the
    // ambiguity harvest, so it works whether or not ambiguities are present. It
    // runs when
    //   - the carry is on (zwd_on),
    //   - the seam window is open (a rolling re-stash), or
    //   - the run writes snapshots (capture only). Nothing is fed back then:
    //     R.pl_zwd_present is record-only on those epochs and no prior is injected.
    if (!(zwd_on || zwd_capture || zwd_seam_live)) return;
    for (size_t i = 0; i < stations.size(); ++i) {
        // The pivot's ZWD is harvested too: R.pl_zwd_present is set for the pivot
        // because the carry inject also runs for it.
        if (est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.pl_zwd_present) continue;
        const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
        Eigen::VectorXd xr, vard;
        try {
            xr = est.reconstructStation(est_idx, x_s);
            if (!carry_cpl_cond)   // the coupling carry uses the pure conditional variance (N_rr⁻¹)_cc
                vard = est.conditionalLocalVarianceDiag(
                    est_idx, {R.pl_zwd.col}, plan.shared_datum);
        } catch (...) { continue; }
        if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
        const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
        if (!std::isfinite(value_abs)) continue;
        double var;
        std::vector<CoupledShared> coupling;
        // In-block route: when this station's harvest put the ZWD column in `cols`
        // (and only then), the ZWD is harvested through the same cross-gather as
        // every ambiguity column (one call, one solve), and its row is written at
        // the last block index, cols.size() - 1. zblk is null on every other
        // station-epoch, so the call is then the same as without the knob.
        const std::shared_ptr<PlHarvestBlock> zblk =
            zwd_blk_on ? blocks[i].segmentBlock(BlockSegment::Zwd) : nullptr;
        const std::vector<Eigen::Index>* zcross_cols =
            zblk ? &blocks[i].cols : nullptr;
        Eigen::VectorXd zcross;
        if (carry_cpl_cond) {
            auto hc = networkHarvestColumn(run, ep, border, est_idx, R.pl_zwd.col, zcross_cols,
                                                            zblk ? &zcross : nullptr);
            var = hc.first; coupling = std::move(hc.second);
        } else {
            var = (vard.size() > 0) ? vard(0) : -1.0;
        }
        if (!(var > 0.0) || !std::isfinite(var)) continue;
        // Prior-residual census row for the ZWD carry. The ZWD carry has no slip,
        // jump or innovation screen of its own, so this census is how its prior is
        // checked against the posterior. `folded` is recorded directly
        // (R.pl_zwd_injected, set by the PASS B injector) and needs no reproduction.
        // The prior is read before the stash below overwrites it. The row is gated
        // on an inject-capable epoch (carry on or seam window open), so a
        // capture-only run adds no rows and its census equals that of a run without
        // capture.
        if (prires_zwd && prires_est.enabled() && (zwd_on || zwd_seam_live)) {
            const auto* zit = net::get(sta_rows, StationId{static_cast<int>(i)},
                                       &net::StationState::zwd);
            if (zit != nullptr) {
                const double zgap = pl_now_s - zit->last_seen_s;
                if (zgap >= 0.0 && zgap <= pl_max_gap_s) {
                    NetworkPriorResidual::Input in;
                    in.family   = "zwd";
                    in.station  = static_cast<int>(i);
                    in.e        = zit->value_abs - value_abs;
                    // Use the same q as the injector. Inside the seam window an
                    // armed network.zwd_seam_reseed_q applies there, and a census
                    // computed against a variance no row used would report the
                    // `zwd` family's σ̂₀ off by the ratio of the two q values.
                    in.fold_var = zit->var
                                + ctx.knobs.zwdQEff(zwd_seam_live)
                                      * std::max(0.0, zgap);
                    in.post_var = var;
                    in.gap_s    = zgap;
                    in.folded   = R.pl_zwd_injected;
                    // A ZWD carry within its horizon that did not inject is not a
                    // mechanism response (nothing fired on it): the injector had
                    // no live column or a non-positive variance. It is classified
                    // Clean, and the mech tag records "not_injected".
                    in.label = PriorResidualLabel::Clean;
                    in.mech  = R.pl_zwd_injected ? "" : "not_injected";
                    prires_est.addRow(in);
                }
            }
        }
        // Resume coupling census, ZWD half. ZWD rows restored from a snapshot are
        // counted like the leveling rows (n_resume_cpl_zrows) and arm the same
        // resume.cpl_census latch. A census that covered only the leveling side
        // would read clean on a leg whose ZWD regression was stale, because it
        // never looked. Same measure-only body as the leveling carry's census; the
        // restored entry is read before the stash below moves `coupling` out.
        if (ctx.carry.resume.cpl_census && carry_cpl_cond
            && !coupling.empty()) {
            const auto* zold = net::get(sta_rows, StationId{static_cast<int>(i)},
                                        &net::StationState::zwd);
            if (zold != nullptr && !zold->coupling.empty()) {
                networkResumeCouplingCensusRow(run, *zold, coupling);
            }
        }
        net::put(sta_rows, StationId{static_cast<int>(i)}, &net::StationState::zwd) =
            LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
        networkSharedHistoryNoteEntry(shx_h, est_idx, R.pl_zwd.col, SharedHistoryMap::Zwd,
                                      static_cast<int>(i));   // shared history; no-op when off
        // In-block route: the last row of the station's harvest block is the ZWD
        // column's own cross-covariance with every carried column (its last entry
        // equals `var`, the pure conditional). The stash then points at the same
        // PlHarvestBlock as the ambiguity entries, so next epoch's inject pairs
        // ZWD with ambiguity by pointer equality, as it pairs ambiguity with
        // ambiguity. A ZWD whose harvest failed above does not reach here: its
        // entry keeps blk = nullptr, and every pair with it is zeroed and counted
        // (pl_blk_zwd_missing_cross), which is the safe degrade.
        if (zblk && zcross.size() == zblk->C.rows()) {
            const Eigen::Index zrow = zblk->C.rows() - 1;   // the appended column
            zblk->C.row(zrow) = zcross.transpose();
            auto& zent = net::put(sta_rows, StationId{static_cast<int>(i)}, &net::StationState::zwd);
            zent.blk     = zblk;
            zent.blk_idx = static_cast<int>(zrow);
        }
    }
    // The horizon comes from the parameter registry (network.phase_leveling_max_gap_s,
    // the same value as pl_max_gap_s).
    carryGapPrune(sta_rows, &net::StationState::zwd, pl_now_s,
                  ctx.params.spec(NetworkParamKind::Zwd).gap_horizon_s,
                  [&](const auto&, const auto&) {
                      ++ctx.counters.n_cr_zwd_gap_prune;   // same gap test as the inject
                      prmRespond(ctx.counters.prm_resp, ResponseSite::ZwdGapPrune);
                  });
    networkZwdSeamWindowStep(run, ep);
}

void networkZwdSeamWindowStep(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run, the epoch and the harvest ----
    NetworkRunContext& ctx = run.ctx;
    std::vector<NetStation>& stations = run.setup.stations;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    double pl_now_s = ep.clock.pl_now_s;
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    // ---- End of the bindings ----
    // Seam-window bookkeeping (network.zwd_seam_reseed). Count this epoch's actual
    // folds (R.pl_zwd_injected is the injector's own record), advance the
    // retirement clock, and log the end of the window when its horizon is used up.
    // The ARMED line logged at the warm resume marks the window's start. Serial
    // section: the parallel produce phase only reads zwd_seam_ep_left.
    if (!zwd_seam_live) return;
    long long seam_folds = 0;
    for (size_t i = 0; i < stations.size(); ++i)
        if (est_index_of[i] >= 0 && asm_res[i].pl_zwd_injected) ++seam_folds;
    ctx.counters.n_zwd_seam_folds += seam_folds;
    if (seam_folds > 0) ++ctx.counters.n_zwd_seam_epochs;
    if (--ctx.carry.zwd_seam_ep_left == 0) {
        // No logger is passed in, so look it up on each call and test it for null.
        // Do not cache it in a static or use it unchecked: spdlog::get returns null
        // for an unknown name, and spdlog::drop_all() clears registered loggers.
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

// ---- End of the harvest ----

// ---- Run-end report ----

// Run-end report: the seam-scoped ZWD re-seed.
int networkZwdSeamRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& C = v.ctx.counters;

    // Telemetry for network.zwd_seam_reseed. An armed knob with 0 folds means the
    // window never opened (no warm resume, or the snapshot had no ZWD rows) or
    // every fold was skipped (stale entries, or the carry not warm). The ARMED or
    // INERT line logged at the resume says which.
    if (config_.networkZwdSeamReseed())
        log->info("Network (ZWD seam re-seed): {} station-fold(s) over {} epoch(s) of the "
                  "post-resume window (horizon {} epochs).{}",
                  C.n_zwd_seam_folds, C.n_zwd_seam_epochs,
                  config_.networkZwdSeamReseedEpochs(),
                  C.n_zwd_seam_folds == 0
                      ? "  ⚠ 0 folds under an ARMED knob — the wiring was inert this "
                        "run; check the resume log for the ARMED/INERT line."
                      : "");
    return 0;
}

} // namespace GnssProcessor
