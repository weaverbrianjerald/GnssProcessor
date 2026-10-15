/**
 * @file NetworkBorderPrior.cpp
 * @brief @internal The priors folded on the shared (border) columns of the solve, and the
 *        inputs of the exact coupling update, which shares the D2 fold's per-key decision.
 *
 * Folded by networkSolveGlue (filter/NetworkSolveGlue.cpp) before the null-space rank gate:
 *   networkInjectShVtecPrior      the SH-VTEC prior (Kaula bound and temporal random walk);
 *   networkInjectPinBridgePrior   the eq34 pin-bridge rows.
 * Folded by networkSolveGlue after the gate:
 *   networkInjectD2CarryPrior     the border carry fold (D2 diagonal or C1 dense), with
 *                                 its file-local READMIT helper readmitNoteSeed;
 *   networkInjectClockOscPrior    the two-state clock-model prior.
 * Called by the driver (app/ApplicationNetwork.cpp) before PASS B:
 *   networkCouplingExactPrepare   card CTU: the exact coupling update's inputs, built from
 *                                 the stash the D2 fold reads and the fold's own per-key
 *                                 decision.
 * File order: SH-VTEC, pin bridge, readmitNoteSeed, D2, coupling preparation, clock
 * model.  The stash the D2 fold reads is written by networkStashCarry
 * (NetworkBorderStash.cpp).
 *
 * networkInjectShVtecPrior, networkInjectD2CarryPrior and networkInjectClockOscPrior open
 * with an alias block (decomposition INC-4) that binds the
 * driver's former local names to members of the context, so the relocated code keeps those
 * names.
 *
 * One of the seven files that define the network run context's functions: card NOM3 step
 * HV0 split them out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/app/NetworkRunContext.hpp.
 */

#include "app/NetworkRunContext.hpp"

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkClockAnchor.hpp"      // guarded temporal clock anchor (card Q3)
#include "filter/NetworkCarryDisposition.hpp" // the decided-once fold disposition (card PRM phase 1b)
#include "filter/NetworkPivotHandover.hpp"   // datumChangeAffectsKey (handover scope predicate)
#include "filter/StateKeySchema.hpp"         // StateKeys::parseAmbiguityKey
#include "math/TimeUtils.hpp"                // TimeUtils::continuousGpsSeconds

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "app/NetworkSetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

void networkInjectShVtecPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                              const GnssConfig& config_, NetworkEstimator& est,
                              const SphericalHarmonicIonosphere* sh_model,
                              const SphericalHarmonicIonosphere* sh_model2,
                              bool datum_changed, double dt, bool within_epoch_armed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& prev_carry = ctx.carry.prev_carry;
    auto& cap_priors = ws.cap_priors;

    // --- SH-VTEC prior (Kaula + temporal), folded before the gate -----------
    // The SH coefficients are shared columns in the reduced normals.  Without a bound
    // on each one, a coefficient whose IPP support has rotated over an ocean or polar
    // gap is near-null and trips the all-or-nothing rank gate.  A degree-dependent
    // Kaula prior (σ_n = kaula_scale/n; a₀₀ gets a loose climatology prior, so the mean
    // VTEC stays data-determined and is not clamped to an external value) keeps every
    // coefficient PSD-bounded, so declared_dimension stays = #constellations regardless
    // of coverage.  The same prior carries the field forward in time as a bounded
    // random walk (anchor the previous estimate, inflate by the per-degree q, never
    // exceed the Kaula bound).  It is folded through addSharedPrior before the gate, so
    // the gate sees the regularized SH block; the clock/bias D2 carry-forward, by
    // contrast, stays after the gate, because it would otherwise fill the clock gauge
    // that the datum pins.
    if (config_.estimateNetworkIonoSh() && sh_model && est.numShared() > 0) {
        std::map<std::tuple<int, int, int, char>, const CarryPrior*> sh_carry;
        // Scope decision (RTA-2): the SH-VTEC carry skip stays global, on purpose, even
        // under network.scoped_handover_response.  The SH coefficients are shared
        // columns without a constellation, so no scoped subset exists for them; and
        // although the pure clock-gauge S-transform leaves the iono columns invariant in
        // exact arithmetic, the carried SH posterior was estimated jointly with the
        // old-datum clock/bias states.  The conservative response is therefore kept: on
        // any datum change the random-walk-toward-previous term is dropped for one epoch
        // and the coefficients fall back to the always-on Kaula climatology anchor below
        // (still PSD-bounded and gate-safe), a small, bounded cost that avoids scoping a
        // column that belongs to no constellation.
        if (!datum_changed)
            for (const CarryPrior& cp : prev_carry)
                if (cp.kind == SharedParamKind::VtecSh)
                    sh_carry.emplace(
                        std::make_tuple(cp.sh_layer, cp.sh_n, cp.sh_m, cp.sh_trig), &cp);
        // Card PRM phase 3a: the carried SH entries this datum change leaves unfolded this
        // epoch (key-epochs; a skipped epoch keeps them for the next).
        if (datum_changed) {
            long long prm_n_sh = 0;
            for (const CarryPrior& cp : prev_carry)
                if (cp.kind == SharedParamKind::VtecSh) ++prm_n_sh;
            prmRespond(ctx.counters.prm_resp, ResponseSite::ShRetire, prm_n_sh);
        }

        // Dual-layer (Iono-2): the same mean-reverting prior recipe per shell, with a
        // per-layer Kaula scale, q and a₀₀ anchor.  The layer-2 a₀₀ anchor is tight on
        // purpose: the inter-layer split of the mean VTEC is nearly null from
        // ground-only geometry (only the mapping-function difference of the two shell
        // heights observes it), while the total mean stays data-determined through
        // layer 1's loose anchor.
        std::vector<NetworkEstimator::SharedPrior> sh_priors;
        for (const ShLayer& lay :
             shLayers(shc, sh_model, sh_model2)) {
            sh_priors.reserve(sh_priors.size() + lay.model->coeffs().size());
            for (const auto& c : lay.model->coeffs()) {
                const double kaula_sigma = (c.n == 0)
                    ? lay.mean_prior_sigma_tecu
                    : lay.kaula_scale_tecu / static_cast<double>(c.n);
                const double kaula_var = kaula_sigma * kaula_sigma;
                const double clim = (c.n == 0 && c.m == 0 && c.trig == 'C')
                    ? lay.mean_prior_tecu : 0.0;   // a₀₀ → mean VTEC; others → 0

                // Mean-reverting (first-order Gauss-Markov) prior: always apply the
                // Kaula climatology anchor (toward clim, σ = kaula_sigma) and, when a
                // posterior was carried forward, a random-walk anchor (toward the
                // previous estimate, inflated by the per-degree q).  Combining them in
                // information space pulls every coefficient back toward climatology
                // each epoch, so the field cannot random-walk away (the design's
                // "unobserved coefficients are pulled to their prior", §6.1), while the
                // random-walk term keeps temporal continuity for well-observed ones.  A
                // pure random walk toward the previous value, with no climatology
                // term, lets the global field drift to unphysical magnitudes; the
                // climatology term bounds it.
                double info  = 1.0 / kaula_var;       // climatology Tikhonov (always on)
                double sumiv = info * clim;
                const auto it =
                    sh_carry.find(std::make_tuple(lay.layer, c.n, c.m, c.trig));
                if (it != sh_carry.end() && std::isfinite(it->second->value)
                    && it->second->var > 0.0) {
                    const double q_deg = (c.n == 0)
                        ? lay.q_per_degree
                        : lay.q_per_degree / static_cast<double>(c.n * c.n);
                    const double var_rw = it->second->var + q_deg * std::max(0.0, dt);
                    if (var_rw > 0.0) {
                        const double info_rw = 1.0 / var_rw;
                        info  += info_rw;
                        sumiv += info_rw * it->second->value;
                    }
                }
                if (!(info > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::VtecSh;
                p.sh_n = c.n; p.sh_m = c.m; p.sh_trig = c.trig; p.sh_layer = lay.layer;
                p.value = sumiv / info; p.info = info;
                sh_priors.push_back(p);
            }
        }
        est.contributionKind(ContributionKind::ShPrior);   // card NOM N2a
        est.addSharedPrior(sh_priors);
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, sh_priors,
                                 ContributionKind::ShPrior);
    }
}

void networkInjectPinBridgePrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                 NetworkEstimator& est, bool within_epoch_armed,
                                 const std::shared_ptr<spdlog::logger>& log) {
    // --- eq34 reattach hygiene (INC-2): the time-linked phase datum ----------
    // The header explains why this is folded before the gate.  Inert (no allocation,
    // no row) unless a bridge is open, which requires network.phase_datum_time_linked.
    net::SatSignalTable& sigs = ctx.entities.sat_signals;   // pin_bridge (card NOM step N4d)
    const std::size_t n_bridges = net::present(sigs, &net::SatSignalState::pin_bridge);
    if (!ctx.knobs.pin_bridge_on || n_bridges == 0) return;
    std::vector<NetworkEstimator::SharedPrior> bp;
    bp.reserve(n_bridges);
    // In (satellite, signal) order, the order of the old "sat_signal" keys (O1).
    net::forEach(sigs, &net::SatSignalState::pin_bridge, [&](const SatSignalId& id, net::PinBridge& br) {
        if (!std::isfinite(br.value)) return;
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::PhaseBias;
        p.sat    = net::satKey(id.sat);
        p.signal = net::signalKey(id.signal);
        p.value  = br.value;
        p.info   = ctx.knobs.pin_bridge_info;
        bp.push_back(p);
        ++br.folds;                       // staleness clock (bounded by bridge_max_ep)
        ++ctx.counters.hyg.bridge_folds;
    });
    if (bp.empty()) return;
    est.contributionKind(ContributionKind::PinBridge);   // card NOM N2a
    est.addSharedPrior(bp);   // skips keys absent from this epoch's layout
    if (within_epoch_armed)
        networkCapturePriors(ws.cap_priors, ws.cap_prior_kinds, bp,
                             ContributionKind::PinBridge);
    // Margin check, sampled once (the analogue of card KP's K2): a datum row whose
    // information sits at the rank-gate cutoff either trips the gate or survives by
    // coincidence.  The bridge is a datum row, so it has to sit well above the cutoff;
    // the actual margin is reported rather than assumed from the default sigma.  The
    // reduced normals here already contain the row just folded, which is what the gate
    // is about to judge.
    if (!ctx.counters.hyg.bridge_margin_logged) {
        ctx.counters.hyg.bridge_margin_logged = true;
        const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::EigenvaluesOnly);
        if (es.info() == Eigen::Success && es.eigenvalues().size() > 0) {
            const double lmax = es.eigenvalues()(es.eigenvalues().size() - 1);
            const double cutoff = est.rankGateTol() * std::max(lmax, 1.0);
            const double decades = (cutoff > 0.0 && ctx.knobs.pin_bridge_info > 0.0)
                ? std::log10(ctx.knobs.pin_bridge_info / cutoff) : 0.0;
            ctx.counters.hyg.bridge_margin_decades = decades;
            if (decades >= 2.0)
                log->info("Network (eq34 hygiene INC-2): bridge row info {:.3e} vs "
                          "rank-gate cutoff {:.3e} — margin {:.1f} decades ABOVE the "
                          "cutoff (sigma {:g} m).",
                          ctx.knobs.pin_bridge_info, cutoff, decades,
                          ctx.knobs.pin_bridge_sigma);
            else
                log->warn("Network (eq34 hygiene INC-2): bridge row info {:.3e} sits "
                          "only {:.1f} decades above the rank-gate cutoff {:.3e} — "
                          "BELOW the Card-KP K2 minimum of 2.0.  A datum row at the "
                          "cutoff is counted as a NULL direction, so the gate will "
                          "withhold the very epochs the bridge exists to save.  "
                          "TIGHTEN network.phase_datum_bridge_sigma_m (currently {:g} "
                          "m) or re-place network.rank_gate_tol from this config's "
                          "estimability dump.",
                          ctx.knobs.pin_bridge_info, decades, cutoff,
                          ctx.knobs.pin_bridge_sigma);
        }
    }
}

namespace {

// READMIT continuity: whether a parked entry's shared column is registered in this epoch's
// layout is carryColumnPresent (filter/NetworkCarryDisposition, card PRM phase 1b), the one
// predicate the fold's decision and the stash's park share.

/// READMIT continuity: bookkeeping and the re-coupling-edge event for one column seeded
/// from a parked pre-gap realization at the D2 install (standing invariant #7: the event
/// records the edge's identity, its evidence gap, and the variance it re-coupled at).
void readmitNoteSeed(NetworkRunContext& ctx, NetworkRunTraces& traces, const GnssTime& t,
                     const CarryPrior& cp, double seed_gap, double horizon_s,
                     double var_eff, bool anchored) {
    const bool clk = cp.kind == SharedParamKind::Clock;
    if (clk) {
        ++ctx.counters.n_readmit_seeded_clk;
        if (anchored) ++ctx.counters.n_readmit_seeded_anch;
        net::putSat(ctx.entities, cp.sat, &net::SatelliteState::readmit_seeded_s) = ctx.carry.readmit_now_s;
    } else {
        ++ctx.counters.n_readmit_seeded_cb;
    }
    ctx.carry.readmit_seed_check.push_back({cp.kind, cp.sat, cp.signal, cp.value, var_eff});
    traces.emitEvent(t, "READMIT", clk ? "seed_clock" : "seed_code_bias", cp.sat, cp.signal,
                     seed_gap, horizon_s, "seed",
                     fmt::format("value={:.4f};var_eff={:.3e};anchored={}", cp.value, var_eff,
                                 anchored ? 1 : 0));
}

}  // namespace

void networkInjectD2CarryPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                               NetworkRunTraces& traces, const GnssTime& t,
                               NetworkEstimator& est, bool datum_changed,
                               const std::set<char>& changed_consts, double dt,
                               bool within_epoch_armed,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    const bool   fullcov_on = ctx.knobs.fullcov_on;
    // The per-key rules (the eq34 and anchored-election re-reference sets, the clock
    // oscillator, the registry's process noise, the reinit variance, the B4 floors and the
    // VCE apply factor) are in the decision made before PASS B
    // (filter/NetworkCarryDisposition, card PRM phase 1b), whose carryFoldVarEff is the one
    // chain both branches below fold through, so the diagonal and the C1 dense fold cannot
    // drift apart: the dense branch's diagonal is var_eff, and the marginal prior on any one
    // state has to be identical either way.
    // Card PRM phase 1b: the per-key read of the decision made once before PASS B; see
    // prmFoldKey below, after the predicates and the epoch clocks it reads.
    bool prm_reexpressed_epoch = false;
    const auto prmTally = [&](CarryPrior& cp, CarryDisposition disp) {
        cp.fold.disposition = disp;
        const int kk = static_cast<int>(cp.kind);
        ++ctx.counters.prm_disp[kk][static_cast<int>(disp)];
        if (carryDispositionFolds(disp) && cp.fold.departed) ++ctx.counters.prm_departed[kk];
    };
    auto& cap_priors = ws.cap_priors;
    auto& cap_dense_keys = ws.cap_dense_keys;
    auto& cap_dense_cov = ws.cap_dense_cov;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // Card SCL: which clones carry row information, read before any prior is folded on a
    // clone column (filter/NetworkClockClone).  No-op off.
    networkClockCloneFoldBegin(ctx, est);
    // The guarded temporal clock anchor sets the clock rows' fold variance in both
    // branches below.  The per-satellite elapsed time it inflates over is continuous
    // GPS seconds, not the driver's inter-epoch dt (see the seam header: every skip path
    // advances dt while the carry survives).  Untouched and unallocated when off.
    const bool   cta_on    = ctx.knobs.cta_on;
    const double cta_now_s = cta_on ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (cta_on) clockAnchorBeginEpoch(ctx);
    // Scope predicate (RTA-2), re-declared over the explicit (datum_changed,
    // changed_consts) parameters, with the same read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };
    // The half of the S-transform (queue 1(b), INC-1) that changes the solve.  When the
    // transform re-expressed this constellation's carry (ctx.carry.sts_consts, filled by
    // networkHandoverStransform earlier this epoch), the prior is valid in the incoming
    // datum and folds: the retired-datum skip below is released for exactly those keys,
    // and the retained rows are counted (invariant #4: transforms applied with
    // sts_rows_retained == 0 mean this release is mis-wired).  The set is empty on every
    // epoch without a handover and whenever the knob is off, so the off path is
    // byte-identical.
    const auto sts_released = [&](const std::string& sat) {
        return !ctx.carry.sts_consts.empty() && !sat.empty()
            && ctx.carry.sts_consts.count(letterOfSat(sat)) > 0;
    };
    // READMIT continuity (E2 seed-first re-admission, network.readmit_seed_horizon_s).
    // Set up the epoch clock that the stash (which has none of its own) and the guard
    // read, clear this epoch's seed check list, and apply the frame drop: a clock-datum
    // change retires a parked entry's frame as it skips the live carry of the changed
    // constellations, but a parked entry cannot be re-stashed fresh next epoch, so it is
    // dropped (and counted) rather than skipped.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    const double now_s = (readmit_h > 0.0) ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (readmit_h > 0.0) {
        ctx.carry.readmit_now_s = now_s;
        ctx.carry.readmit_seed_check.clear();
        if (datum_changed) {
            const auto before = prev_carry.size();
            const auto parked_retired = [&](const CarryPrior& cp) {
                return cp.t_park_s > 0.0
                    && handover_affects(cp.sat)
                    && !sts_released(cp.sat);
            };
            // Card PRM phase 3a: the parked entries the drop below removes, per kind (the
            // same pure predicate, read-only, ahead of the erase).
            for (const CarryPrior& cp : prev_carry) {
                if (!parked_retired(cp)) continue;
                if (cp.kind == SharedParamKind::Clock)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::ClkParkDiscardDatum);
                else if (cp.kind == SharedParamKind::CodeBias)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::CbParkDiscardDatum);
            }
            prev_carry.erase(std::remove_if(prev_carry.begin(), prev_carry.end(),
                                            parked_retired),
                             prev_carry.end());
            ctx.counters.n_readmit_dropped_datum +=
                static_cast<long long>(before - prev_carry.size());
        }
    }
    // Card PRM phase 1b: both branches read each entry's fold decision, made once before
    // PASS B by networkCarryDecide (filter/NetworkCarryDisposition) and read by the coupling
    // rows too: the clauses in the fold's order (the SH prior folds its keys elsewhere; a
    // frame re-reference, the clock oscillator and a park do not fold; a
    // monitor-flagged clock folds at the reinit variance; a seed at q over the gap since its
    // park; a clock under the Q3 anchor at the anchor's increment), the B4 floors, the VCE
    // factor and the switch-on seam's level noise (dense branch).  The fold adds the clause
    // no earlier point can know: a clock-pivot handover retires the changed constellations'
    // datum unless the S-transform re-expressed their carry (recorded as Retired).  It forms
    // the variance on the carried variance it holds, which the S-transform has changed since
    // the rows were built (counted as re-expressed), commits the anchor's per-satellite
    // bookkeeping, records a non-positive variance as Screened, and tallies what it applied.
    // Returns true with the variance to fold; false when the entry does not fold.
    const auto prmFoldKey = [&](CarryPrior& cp, bool dense, double& var_eff,
                                bool& seed_anchored) -> bool {
        if (cp.fold.disposition == CarryDisposition::Undecided) {   // no pass this epoch
            ++ctx.counters.prm_undecided_at_fold;
            cp.fold = carryFoldDecide(ctx, est, cp, dt, cta_now_s, now_s);
        }
        const CarryFoldDecision& d = cp.fold;
        // The SH prior: folded elsewhere, before the gate.
        if (d.disposition == CarryDisposition::Held) {
            prmTally(cp, CarryDisposition::Held);
            return false;
        }
        if (handover_affects(cp.sat)) {
            if (!sts_released(cp.sat)) {               // retired-datum key, scoped mode only (RTA-2)
                if (dense && ctx.knobs.carry_cpl_exact && ctx.carry.cxu_epoch.ok) {
                    const CouplingExactEpoch& xe = ctx.carry.cxu_epoch;
                    const auto pk = xe.p_of_key.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                    if (pk != xe.p_of_key.end()
                        && xe.s_of_p[static_cast<std::size_t>(pk->second)] >= 0)
                        ++ctx.counters.cxu_fold_retired_in_s;   // the rows carried it in S
                }
                prmTally(cp, CarryDisposition::Retired);
                return false;
            }
            ++ctx.counters.sts_rows_retained;          // re-expressed => folds instead (queue 1(b))
        }
        // A re-referenced phase-bias frame (eq34 D4 hygiene, card 3; the anchored election,
        // queue item 1(d)): marginalized for one epoch, its prior belongs to the retired
        // frame.  A clock on the clock oscillator; a parked entry (column absent, or past
        // the seed horizon).
        if (!carryDispositionFolds(d.disposition)) {
            prmTally(cp, d.disposition);
            return false;
        }
        // The satellite clock's temporal constraint commits here, at the fold, so an epoch
        // that ends earlier advances nothing; a monitor-driven reinit is excluded (a stiff
        // anchor would re-freeze what the monitor just loosened).
        ClockAnchorFold cf;
        var_eff = carryFoldVarEff(ctx, cp, d, cp.var, dense, &cf);
        seed_anchored = false;
        if (cta_on && cp.kind == SharedParamKind::Clock
            && d.disposition != CarryDisposition::Reinit) {
            clockAnchorFoldCommit(ctx, cp.sat, cf, cta_now_s, cp.value);
            seed_anchored = cf.anchored;
        }
        if (cp.var != d.var_decided_m2 || cp.value != d.value_decided_m) {
            ++ctx.counters.prm_reexpressed;
            prm_reexpressed_epoch = true;
            ctx.counters.prm_reexpressed_max_dvar = std::max(
                ctx.counters.prm_reexpressed_max_dvar, std::abs(cp.var - d.var_decided_m2));
        }
        if (!(var_eff > 0.0)) {
            prmTally(cp, CarryDisposition::Screened);
            return false;
        }
        prmTally(cp, d.disposition);
        return true;
    };

    // --- Diagonal D2 carry-forward prior (inflated over the gap) --------
    // Applied after the gate (which has to see the data-only gauge structure) and before
    // the solve.  The prior anchors each non-datum shared parameter to its
    // previous-epoch estimate; it is skipped on a pivot handover, when the carried values
    // belong to a retired datum.  Under network.scoped_handover_response (RTA-2) only the
    // changed constellations' datum is retired: a per-constellation clock gauge is
    // independent of its siblings, so carries of unaffected constellations remain valid
    // and fold normally, and the per-key skip below drops exactly the retired-datum keys.
    // Off reproduces the legacy all-or-nothing skip byte for byte (the outer gate reduces
    // to !datum_changed, and the per-key skip cannot trigger inside it).
    if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on) {
        std::vector<NetworkEstimator::SharedPrior> priors;
        priors.reserve(prev_carry.size());
        for (CarryPrior& cp : prev_carry) {
            // The entry's decision (card PRM phase 1b, prmFoldKey above), which holds the
            // clauses this loop applied before it.  READMIT (E2 seed-first re-admission): a
            // parked entry is a departed satellite's pre-gap realization; with its column
            // re-registered it seeds the column at var + q*gap, the Q3 anchor folds over its
            // own per-satellite elapsed time bounded by the seed horizon, and the guard
            // re-validates.  A clock-monitor-flagged satellite folds with the large reinit
            // variance (a soft large-q reinitialization): the weak prior still anchors the
            // parameter at its previous value, which limits the collective datum drift a hard
            // refloat causes, but is loose enough for the data to move a frozen clock back
            // toward truth.  The B4 floor of the phase-bias / code-bias carry caps the
            // information so a stiff zero-q carry cannot lock in a biased product.
            double var_eff = 0.0;
            bool seed_anchored = false;
            if (!prmFoldKey(cp, /*dense=*/false, var_eff, seed_anchored)) continue;
            const double seed_gap = cp.fold.seed_gap_s;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;
            if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                clk_prior_info[cp.sat] = p.info;   // scalar clock carry info on this column (B7)
            priors.push_back(p);
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
        }
        // With the gauge-free fold armed (card Q3 §2.1), the clock entries are folded
        // with the per-constellation common direction marginalized out, so the anchor
        // constrains the estimable between-satellite differences and leaves the gauge to
        // the S-basis datum.  Non-clock entries fold identically either way, and the off
        // path is the original single call.
        const ContributionKind carry_kind = ctx.knobs.carry_gauge_free
                                                ? ContributionKind::BorderCarryGaugeFree
                                                : ContributionKind::BorderCarryDiagonal;
        est.contributionKind(carry_kind);   // card NOM N2a
        if (ctx.knobs.carry_gauge_free) {
            const std::size_t gf = est.addSharedPriorClockGaugeFree(priors);
            ctx.counters.cta_gf_keys += static_cast<long long>(gf);
            if (gf == 0) ++ctx.counters.cta_gf_inert;
        } else {
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
        }
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, priors, carry_kind);
    } else if ((!datum_changed || scoped_handover) && !prev_carry.empty()) {
        // Dense carry (C1, network.shared_carry_full_cov): fold the carried block as one
        // dense information prior Λ = (P_prev + Q)⁻¹, the information-form KF time update
        // on the shared border, which keeps the cross-covariances (most importantly
        // clock↔phase-bias) that the diagonal branch above discards.  Entry selection,
        // per-key skip rules and the per-key var_eff adjustments (inflate_q reinit, q·dt
        // inflation, B4 floors) are identical to the diagonal branch; the only addition
        // is the retained off-diagonals.  Keys with no matrix row (the first fold after a
        // warm resume, or after a stash-side non-finite screen) fold through the legacy
        // diagonal path, so the mode degrades per key and does not fail.  Keys skipped by
        // rule are marginalized (their rows and columns of P are not selected), which is
        // the exact treatment for a departed or retired parameter.
        std::vector<NetworkEstimator::SharedPrior> dkeys;     // dense-eligible set
        std::vector<std::size_t> drow;                        // row in prev_carry_cov.P
        std::vector<double> dvar_eff;                         // per-key var_eff (diag + fallback)
        std::vector<char> dinflate;                           // inflate_q per dense key
        std::vector<char> dreexp;                             // re-expressed since the decision (card PRM phase 1b)
        std::vector<double> dpfq_rate, dpfq_dt;               // card PFQ: the pivot's rate and the key's interval
        std::vector<NetworkEstimator::SharedPrior> leftovers; // no matrix row → diagonal fold
        dkeys.reserve(prev_carry.size());
        drow.reserve(prev_carry.size());
        dvar_eff.reserve(prev_carry.size());
        dinflate.reserve(prev_carry.size());
        dreexp.reserve(prev_carry.size());
        for (CarryPrior& cp : prev_carry) {
            // The entry's decision (card PRM phase 1b, prmFoldKey above), the same per-key
            // rules as the diagonal branch, so the marginal prior on any one state is
            // identical on both branches (the property that makes the guard statistic
            // path-independent; the dense fold's off-diagonals are kept on purpose, so the
            // anchor does not re-create the independence assumption C1 removed on the state
            // it makes stiffest, SPEC section 2.1).  A READMIT seed has no dense row (it was
            // never in the stashed block), so it folds through the diagonal leftovers below.
            // Card CTU G5: a switch-on seam's level noise is on the diagonal (inside the
            // decision's variance) and between the satellite's clock and phase-bias keys
            // below.
            double var_eff = 0.0;
            bool seed_anchored = false;
            if (!prmFoldKey(cp, /*dense=*/true, var_eff, seed_anchored)) continue;
            const double seed_gap = cp.fold.seed_gap_s;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
            const auto rit = prev_carry_cov.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (rit == prev_carry_cov.row.end()) {
                if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                    clk_prior_info[cp.sat] = p.info;   // diagonal-carry info, no dense row (B7)
                leftovers.push_back(p);
                continue;
            }
            dkeys.push_back(p);
            drow.push_back(rit->second);
            dvar_eff.push_back(var_eff);
            dinflate.push_back(cp.inflate_q ? 1 : 0);
            dreexp.push_back((cp.var != cp.fold.var_decided_m2 || cp.value != cp.fold.value_decided_m) ? 1 : 0);
            dpfq_rate.push_back(cp.fold.pfq_rate_m2ps);
            dpfq_dt.push_back(cp.fold.dt_applied_s);
        }
        const std::size_t kd = dkeys.size();
        // Card CTU: the coupling rows were built in PASS B from the same decision
        // (networkCouplingExactPrepare reads it, card PRM phase 1b).  A key in S that a
        // clock-pivot handover re-expressed after the rows were built folds here on the
        // re-expressed variance, while its rows were conditioned on the decided one; it is
        // counted (the retired ones are counted in prmFoldKey).  Read-only.
        if (ctx.knobs.carry_cpl_exact && ctx.carry.cxu_epoch.ok) {
            const CouplingExactEpoch& xe = ctx.carry.cxu_epoch;
            if (datum_changed) ++ctx.counters.cxu_fold_handover_epochs;
            for (std::size_t a = 0; a < kd; ++a) {
                const std::size_t p = drow[a];
                if (p < xe.s_of_p.size() && xe.s_of_p[p] >= 0 && dreexp[a])
                    ++ctx.counters.cxu_fold_reexpressed_in_s;
            }
        }
        if (kd > 0) {
            // P_eff over the dense set: the subset of the stashed posterior covariance,
            // with diagonal := var_eff (the stashed diagonal equals cp.var by
            // construction, so this is the legacy q·dt inflation plus the B4 floor).  A
            // clock-monitor-flagged key is decorrelated before its diagonal becomes the
            // reinit variance: its old cross-covariances are stale once the variance is
            // replaced, and a swapped diagonal under live off-diagonals could make P_eff
            // indefinite.
            Eigen::MatrixXd cov_eff(kd, kd);
            for (std::size_t a = 0; a < kd; ++a)
                for (std::size_t b = 0; b < kd; ++b)
                    cov_eff(a, b) = prev_carry_cov.P(drow[a], drow[b]);
            for (std::size_t a = 0; a < kd; ++a) {
                if (dinflate[a]) {
                    cov_eff.row(static_cast<Eigen::Index>(a)).setZero();
                    cov_eff.col(static_cast<Eigen::Index>(a)).setZero();
                }
                cov_eff(a, a) = dvar_eff[a];
            }
            // Card CTU G5: the off-diagonal half of the seam's level noise (its diagonal is
            // in dvar_eff): v between every pair of the satellite's clock and phase-bias
            // keys, so the phase-visible combinations get no noise.
            if (!ctx.carry.clm_seam_q.empty()) {
                const auto lvl = [&](std::size_t a) {
                    const auto& k = dkeys[a];
                    return !dinflate[a] && (k.kind == SharedParamKind::Clock
                                            || k.kind == SharedParamKind::PhaseBias);
                };
                for (std::size_t a = 0; a < kd; ++a) {
                    if (!lvl(a)) continue;
                    const auto sq = ctx.carry.clm_seam_q.find(dkeys[a].sat);
                    if (sq == ctx.carry.clm_seam_q.end()) continue;
                    for (std::size_t b = 0; b < kd; ++b)
                        if (b != a && lvl(b) && dkeys[b].sat == dkeys[a].sat)
                            cov_eff(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) += sq->second;
                }
            }
            // Card PFQ: the pairwise half of the pivot-frame clock noise.  The diagonal half
            // is inside dvar_eff through the decision; two clocks of one constellation that
            // took it share the pivot's walk over the overlap of their intervals.  A reinit
            // key took no term (its rate is 0), so its decorrelated row stays zero.
            if (ctx.knobs.pfq_on) {
                for (std::size_t a = 0; a < kd; ++a) {
                    if (!(dpfq_rate[a] > 0.0) || dkeys[a].kind != SharedParamKind::Clock) continue;
                    for (std::size_t b = a + 1; b < kd; ++b) {
                        if (!(dpfq_rate[b] > 0.0) || dkeys[b].kind != SharedParamKind::Clock
                            || systemOfSat(dkeys[b].sat) != systemOfSat(dkeys[a].sat))
                            continue;
                        const double v = pivotFrameCross(dpfq_rate[a], dpfq_dt[a], dpfq_dt[b]);
                        const auto ia = static_cast<Eigen::Index>(a), ib = static_cast<Eigen::Index>(b);
                        cov_eff(ia, ib) += v;
                        cov_eff(ib, ia) += v;
                        ++ctx.counters.pfq.dense_pairs;
                    }
                }
            }
            try {
                Eigen::VectorXd lam;
                // Card SCL: the joint fold over the stand-ins (each cloned clock stood in for
                // by its clone) and the clones' walk factors when a clone stands in this
                // epoch, which also writes the B7 information; card CTU's fold otherwise.
                // Off, the clone fold returns false at once and the fold below is unchanged.
                const bool ccl_fold = networkClockCloneDenseFold(
                    ctx, ws, est, dkeys, drow, dvar_eff, dinflate, dreexp, cov_eff,
                    within_epoch_armed, anchor_info_trace.is_open() ? &clk_prior_info : nullptr);
                if (!ccl_fold) {
                    est.contributionKind(ContributionKind::BorderCarryDense);   // card NOM N2a
                    est.addSharedPriorDense(dkeys, cov_eff,
                                            anchor_info_trace.is_open() ? &lam : nullptr,
                                            ctx.knobs.carry_gauge_free);
                    if (anchor_info_trace.is_open())
                        for (std::size_t a = 0; a < kd; ++a)
                            if (dkeys[a].kind == SharedParamKind::Clock)
                                clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // folded Λ(c,c) (B7)
                }
                if (ctx.knobs.carry_gauge_free) {
                    std::size_t nclk = 0;
                    for (std::size_t a = 0; a < kd; ++a)
                        if (dkeys[a].kind == SharedParamKind::Clock) ++nclk;
                    ctx.counters.cta_gf_keys += static_cast<long long>(nclk);
                    if (nclk < 2) ++ctx.counters.cta_gf_inert;
                }
                if (within_epoch_armed) {
                    cap_dense_keys = dkeys;      // replayed in resolve_dropping
                    cap_dense_cov  = cov_eff;
                }
            } catch (const std::exception& e) {
                // Numerical fallback: fold the same keys as independent diagonal priors
                // with 1/var_eff, the legacy semantics for this epoch.  The next emit
                // re-stashes a fresh block, so the fallback lasts one epoch.
                log->warn("Network (C1): dense carried-covariance fold failed ({}) — "
                          "falling back to the diagonal carry for this epoch.",
                          e.what());
                traces.emitEvent(t, "D2_DENSE", "fallback_diagonal", "", "",
                          static_cast<double>(kd), std::nullopt, "fallback",
                          e.what());   // non-PD, non-finite or conditioning budget; emitEvent sanitizes commas
                // Card CTU: the rows of this epoch were updated against the dense block.
                if (ctx.knobs.carry_cpl_exact) ++ctx.counters.cxu_fold_diag_fallback;
                networkClockCloneNoteFallback(ctx);   // card SCL: the clones are then free or isolated
                for (std::size_t a = 0; a < kd; ++a) {
                    if (anchor_info_trace.is_open() &&
                        dkeys[a].kind == SharedParamKind::Clock)
                        clk_prior_info[dkeys[a].sat] = dkeys[a].info;
                    leftovers.push_back(dkeys[a]);
                }
            }
        }
        // First post-resume fold (§18.12, invariant #7): unless the dense border was
        // restored from the snapshot's QK/QP records (RS-7, see networkTryWarmResume),
        // every carried key misses prev_carry_cov.row here and folds through the legacy
        // diagonal path while the restored coupling rows are already conditional.  That
        // asymmetry lasts one epoch; the census measures its size instead of assuming
        // it.  It is read here, before the gauge-free / plain addSharedPrior branch
        // below, so the count covers the same population on both fold routes.
        if (ctx.carry.resume_cpl_census_pending)
            ctx.counters.n_resume_cpl_diag_border =
                static_cast<long long>(leftovers.size());
        // Leftovers are the keys the dense fold could not take (no matrix row after a
        // warm resume, or a non-finite screen; card Q3 §2.1).  Under the gauge-free fold
        // they form their own group, and a constellation that contributes one leftover
        // clock is entirely gauge, so nothing is folded for it.  This is conservative by
        // construction: the fallback can under-anchor but cannot re-introduce gauge
        // information.
        // Card NOM N2a: the leftovers are the border carry folded diagonally.
        const ContributionKind leftover_kind = ctx.knobs.carry_gauge_free
                                                   ? ContributionKind::BorderCarryGaugeFree
                                                   : ContributionKind::BorderCarryDiagonal;
        est.contributionKind(leftover_kind);
        if (ctx.knobs.carry_gauge_free)
            ctx.counters.cta_gf_keys +=
                static_cast<long long>(est.addSharedPriorClockGaugeFree(leftovers));
        else
            est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, leftovers, leftover_kind);
    }
    // Card PRM phase 3a: a clock-pivot handover in legacy (unscoped) mode skips both fold
    // branches, retiring every carried entry for the epoch; tallied here (counters only, the
    // entries' records are not written).  A held entry keeps its Held tally.
    if (datum_changed && !scoped_handover)
        for (const CarryPrior& cp : prev_carry) {
            const bool held = cp.fold.disposition == CarryDisposition::Held;
            ++ctx.counters.prm_disp[static_cast<int>(cp.kind)][static_cast<int>(
                held ? CarryDisposition::Held : CarryDisposition::Retired)];
        }
    if (prm_reexpressed_epoch) ++ctx.counters.prm_reexpressed_epochs;   // card PRM phase 1b
    // Card CTU G5: the seam's level noise is one epoch's process noise.
    ctx.carry.clm_seam_q.clear();
    // Card SCL: a clone that did not stand in is free when rows reference it and takes an
    // isolated prior when none does (a solve no-op on every other state), whichever branch
    // above ran, including none.  No-op off.
    networkClockCloneFoldEnd(ctx, ws, est, within_epoch_armed);
    // Card SCL2 X2: the registered shared-history columns' N(0, 1) priors, independent of
    // the border (filter/NetworkSharedHistory).  No-op off.
    networkSharedHistoryFold(ctx, ws, est, within_epoch_armed);
}

void networkCouplingExactPrepare(NetworkRun& run, const NetworkEpoch& nep) {
    // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    // The frozen body declares or uses `ep`, so this definition names the parameter nep
    // (specification 3.2).
    NetworkRunContext& ctx = run.ctx;
    const NetworkEstimator& est = run.engines.est;
    const GnssTime& t = nep.se.time;
    double dt = nep.clock.dt;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    CouplingExactEpoch& ep = ctx.carry.cxu_epoch;
    ep = CouplingExactEpoch{};
    if (!ctx.knobs.carry_cpl_exact) return;
    // Card SCL: forget the previous epoch's rows' copy (rebuilt at the end once this epoch's
    // inputs are built).  No-op off.
    networkClockClonePrepare(ctx, {});
    ++ctx.counters.cxu_epochs;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    if (!ctx.carry.cxu_stash_valid || pcc.P.rows() == 0 || pcc.row.empty()) {
        ++ctx.counters.cxu_epochs_nostash;
        return;
    }
    // --- The rows' view of each key: the fold's own decision (card PRM phase 1b) ------
    // Decided once before PASS B (networkCarryDecide) and read here and by the fold.  A key
    // the fold folds is in S at the variance the decision forms on its stashed variance
    // (the dense fold's chain, the switch-on seam's level noise included); a key the fold
    // does not carry goes to D with its route, and one whose column left the layout as
    // departed.  The one clause no point before PASS B can know, a clock-pivot handover,
    // is applied by the fold, which counts the keys the handover re-expressed after these
    // rows were built.  VtecSh never enters the dense block, and a parked or seeding
    // entry has no dense row.
    const auto rowsVar = [&](const CarryPrior& cp, CouplingExactDRoute& route) -> double {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        switch (cp.fold.disposition) {
            case CarryDisposition::Reref:     // a re-referenced phase-bias frame
                route = CouplingExactDRoute::Reref;
                return nan;
            case CarryDisposition::Reinit:    // decorrelated by the fold: q = infinity
                route = CouplingExactDRoute::Reinit;
                return nan;
            case CarryDisposition::Folded:
            case CarryDisposition::Anchored:
            case CarryDisposition::Loose:
                break;
            default:                          // held, on the oscillator, parked, seeding
                route = CouplingExactDRoute::Skipped;
                return nan;
        }
        const double var_eff = carryFoldVarEff(ctx, cp, cp.fold, cp.var, /*dense=*/true);
        if (!(var_eff > 0.0)) {
            route = CouplingExactDRoute::Skipped;
            return nan;
        }
        route = CouplingExactDRoute::Present;
        return var_eff;
    };
    const auto columnOf = [&](const CarryPrior& cp) -> Eigen::Index {
        try {
            switch (cp.kind) {
                case SharedParamKind::Clock:     return est.clockColumn(cp.sat);
                case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cp.sat, cp.signal);
                case SharedParamKind::CodeBias:  return est.codeBiasColumn(cp.sat, cp.signal);
                default:                         return -1;
            }
        } catch (...) {}
        return -1;
    };
    const int np = static_cast<int>(pcc.P.rows());
    ep.P = pcc.P;
    ep.p_of_key.clear();
    ep.s_of_p.assign(static_cast<std::size_t>(np), -1);
    ep.route_p.assign(static_cast<std::size_t>(np), static_cast<int>(CouplingExactDRoute::Skipped));
    ep.clk_p.assign(static_cast<std::size_t>(np), 0);
    ep.cc_p.assign(static_cast<std::size_t>(np), '\0');
    std::vector<const CarryPrior*> cp_of_p(static_cast<std::size_t>(np), nullptr);
    for (const CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind == SharedParamKind::VtecSh) continue;
        const std::string key = sharedCarryKey(cp.kind, cp.sat, cp.signal);
        const auto rit = pcc.row.find(key);
        if (rit == pcc.row.end() || static_cast<int>(rit->second) >= np) {
            // A leftover (the fold's diagonal route): its inflation is the scalar fallback
            // of the rows' terms on it.
            CouplingExactDRoute r;
            const double v = rowsVar(cp, r);
            if (std::isfinite(v)) ep.q_outside[key] = std::max(0.0, v - cp.var);
            continue;
        }
        cp_of_p[rit->second] = &cp;
    }
    std::vector<double> qs, sh;
    for (int p = 0; p < np; ++p) {
        const CarryPrior* cp = cp_of_p[static_cast<std::size_t>(p)];
        if (!cp) continue;                   // a dense row with no carried entry: D (Skipped)
        const std::string key = sharedCarryKey(cp->kind, cp->sat, cp->signal);
        ep.p_of_key[key] = p;
        ep.clk_p[static_cast<std::size_t>(p)] = (cp->kind == SharedParamKind::Clock) ? 1 : 0;
        // The constellation letter is read for clock rows only (their sat is a satellite).
        ep.cc_p[static_cast<std::size_t>(p)]  =
            cp->sat.empty() ? '\0' : letterOfSat(cp->sat);
        CouplingExactDRoute route;
        const double var_eff = rowsVar(*cp, route);
        if (!std::isfinite(var_eff)) {
            ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(route);
            continue;
        }
        const Eigen::Index col = columnOf(*cp);
        if (col < 0) {
            ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Departed);
            continue;
        }
        double q = var_eff - pcc.P(p, p);    // the fold sets the diagonal of P to var_eff
        // (the seam's level noise is inside var_eff through the decision, like the fold's)
        if (q < 0.0) {
            ++ctx.counters.cxu_q_negative;
            q = 0.0;
        }
        ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Present);
        ep.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(col);
        qs.push_back(q);
        sh.push_back(cp->value);
    }
    for (int p = 0; p < np; ++p) {
        const int r = ep.route_p[static_cast<std::size_t>(p)];
        if (r == static_cast<int>(CouplingExactDRoute::Present)) ++ctx.counters.cxu_keys_s;
        else if (r >= 0 && r < kCouplingExactRoutes) ++ctx.counters.cxu_keys_d[r];
    }
    ep.q_s    = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<const Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    ep.stash_s = ctx.carry.cxu_stash_s;
    // Card CTU G5: the off-diagonal half of a switch-on seam's level noise (the fold adds
    // the same between the satellite's clock and phase-bias keys), from the decision's seam
    // noise (a clock or phase bias of a seam satellite that is not a reinit).
    ep.q_off.clear();
    if (!ctx.carry.clm_seam_q.empty()) {
        const Eigen::Index nS = ep.nS();
        const auto seamCp = [&](Eigen::Index a) -> const CarryPrior* {
            const CarryPrior* c = cp_of_p[static_cast<std::size_t>(ep.p_of_s[static_cast<std::size_t>(a)])];
            return (c && c->fold.has_seam) ? c : nullptr;
        };
        for (Eigen::Index a = 0; a < nS; ++a) {
            const CarryPrior* ca = seamCp(a);
            if (!ca) continue;
            for (Eigen::Index b = 0; b < nS; ++b) {
                if (b == a) continue;
                const CarryPrior* cb2 = seamCp(b);
                if (cb2 && cb2->sat == ca->sat)
                    ep.q_off.emplace_back(static_cast<int>(a), static_cast<int>(b), ca->fold.seam_q_m2);
            }
        }
    }
    // Card PFQ: the pairwise half of the pivot-frame clock noise, the same as the dense fold
    // adds (its diagonal half is inside q_s through the decision), so the rows are
    // re-expressed with the fold's own Q_S.
    if (ctx.knobs.pfq_on) {
        const Eigen::Index nS = ep.nS();
        const auto pfqCp = [&](Eigen::Index a) -> const CarryPrior* {
            const CarryPrior* c = cp_of_p[static_cast<std::size_t>(ep.p_of_s[static_cast<std::size_t>(a)])];
            return (c && c->kind == SharedParamKind::Clock && c->fold.pfq_rate_m2ps > 0.0
                    && !c->sat.empty()) ? c : nullptr;
        };
        for (Eigen::Index a = 0; a < nS; ++a) {
            const CarryPrior* ca = pfqCp(a);
            if (!ca) continue;
            for (Eigen::Index b = 0; b < nS; ++b) {
                if (b == a) continue;
                const CarryPrior* cb2 = pfqCp(b);
                if (!cb2 || systemOfSat(cb2->sat) != systemOfSat(ca->sat)) continue;
                ep.q_off.emplace_back(static_cast<int>(a), static_cast<int>(b),
                                      pivotFrameCross(ca->fold.pfq_rate_m2ps, ca->fold.dt_applied_s,
                                                      cb2->fold.dt_applied_s));
                ++ctx.counters.pfq.cxu_pairs;
            }
        }
    }
    // The pivots, re-resolved in this epoch's layout.  A pivot that left the layout keeps
    // no entry, and the rows count the deficit as an orphan.
    bool any_clk = false;
    for (int p = 0; p < np; ++p) any_clk = any_clk || ep.clk_p[static_cast<std::size_t>(p)];
    net::forEach(ctx.entities.constellations, &net::ConstellationState::cxu_pivot,
                 [&](System sys, const std::pair<std::string, double>& pv) {
                     try {
                         const Eigen::Index col = est.clockColumn(pv.first);
                         if (col >= 0) ep.pivot[rinexLetter(sys)] = {col, pv.second};
                     } catch (...) {}
                 });
    if (any_clk && ep.pivot.empty()) ++ctx.counters.cxu_pivot_missing;
    if (!couplingExactFactor(ep)) {
        ++ctx.counters.cxu_epochs_unfactored;
        log->warn("Network (card CTU): P_SS + Q_S over {} carried key(s) did not factor at "
                  "TOW {:.1f}; the coupling rows take the card 1d route this epoch.",
                  ep.nS(), t.tow);
        return;
    }
    ++ctx.counters.cxu_epochs_built;
    // Card SCL: the rows' copy of this epoch with each cloned clock on its clone at Q = 0
    // (filter/NetworkClockClone).  This epoch itself stays as built: card SMH and the fold's
    // counters read it.  No-op off.
    networkClockClonePrepare(ctx, cp_of_p);
}

void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkRunTraces& traces, const GnssTime& t,
                                NetworkEstimator& est,
                                const std::vector<std::string>& registered_clock_sats,
                                bool clk_model_warm, bool datum_changed,
                                const std::set<char>& changed_consts,
                                double pl_now_s, double dt, bool within_epoch_armed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const double clk_model_q1 = ctx.knobs.clk_model_q1;
    const double clk_model_q2 = ctx.knobs.clk_model_q2;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    auto& clk_pred = ws.clk_pred;
    auto& cap_priors = ws.cap_priors;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // Scope predicate (RTA-2), re-declared over the explicit (datum_changed,
    // changed_consts) parameters, with the same read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };

    // --- Two-state oscillator clock carry-forward prior (network.clock_model) -----------
    // The satellite clock's prior comes from the physical clock model: predict
    // [offset, drift] forward and fold the offset (mean = drift-following prediction,
    // info = 1/predicted var).  Because the prediction is tight (cm level), the
    // post-solve innovation exposes a lock-in that the scalar random walk would freeze.
    // Skipped on a pivot handover (retired datum) and for the first warm epoch of each
    // satellite (no prediction until it is seeded).  Under
    // network.scoped_handover_response (RTA-2) a handover retires only the changed
    // constellations' datum, so only their satellites skip the prediction (they re-seed
    // from this epoch's new-datum solve in the post-solve update); oscillators of
    // unaffected constellations predict normally.  Off is byte-identical (the outer
    // gate reduces to !datum_changed, and the per-satellite skip cannot trigger inside
    // it).
    if (clk_model_warm && (!datum_changed || scoped_handover)) {
        std::vector<NetworkEstimator::SharedPrior> clkp;
        clkp.reserve(net::present(ctx.entities.sats, &net::SatelliteState::clock_osc));
        for (const std::string& sat : registered_clock_sats) {
            if (handover_affects(sat)) continue;  // retired-datum satellite, scoped mode only (RTA-2)
            const ClockOsc* it = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::clock_osc);
            if (!it || !it->init) continue;
            // Predict over this satellite's true elapsed time, not the global epoch dt
            // (P0-3).  A satellite absent for several epochs (eclipse exclusion is on in
            // the clock-model config) would otherwise get an hours-old state predicted
            // over one dt of Q, a falsely tight prior.  Past the gap bound its carried
            // state is stale: the prior is dropped (reseed), so the clock re-acquires from
            // data and the a-priori anchor instead of an old value.  (Within the bound,
            // the Q·dt inflation over the true gap de-weights the prior, which limits
            // itself.)
            const double sat_dt = (it->t_last > 0.0)
                                      ? (pl_now_s - it->t_last) : std::max(0.0, dt);
            if (!(sat_dt >= 0.0) || sat_dt > clk_model_max_gap) {
                // Event for the stale-carry reseed, which is otherwise not reported (T5,
                // V2 blind spot).  It is edge-triggered by construction: the update
                // reseeds t_last, so the next epoch is fresh.
                traces.emitEvent(t, "T5_clock_model", "stale_prior_reseed", sat, "", sat_dt,
                          clk_model_max_gap, "reseed", "carry past gap");
                continue;   // stale → no prior, reseed (R11)
            }
            const ClockOscPred pr = clockOscPredict(*it, sat_dt,
                                                    clk_model_q1, clk_model_q2);
            if (!(pr.p00 > 0.0) || !std::isfinite(pr.x0)) continue;
            clk_pred[sat] = pr;
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = sat; p.signal = "";
            p.value = pr.x0; p.info = 1.0 / pr.p00;
            if (anchor_info_trace.is_open()) clk_prior_info[sat] = p.info;  // two-state clock-model prior info (B7)
            clkp.push_back(p);
        }
        est.contributionKind(ContributionKind::ClockOscillator);   // card NOM N2a
        est.addSharedPrior(clkp);
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, clkp,
                                 ContributionKind::ClockOscillator);
    }
}

} // namespace GnssProcessor
