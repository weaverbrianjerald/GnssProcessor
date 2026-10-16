/**
 * @file BorderStash.cpp
 * @brief @internal The epoch-tail stash: the border carry kept for the next epoch's fold,
 *        and the flagged satellites kept for the next emit.
 *
 *   networkRetroUraBookkeeping    the retro-bump flags and the URA inflation horizon of the
 *                                 satellites the QC tiers flagged, called by
 *                                 networkClockQc (network/qc/ClockQc.cpp) after its
 *                                 clock monitor;
 *   networkStashCarry             the border stash (prev_carry, prev_carry_cov) and the
 *                                 READMIT continuity bookkeeping, called by
 *                                 networkStashStatus (network/diag/StashStatus.cpp) and
 *                                 directly by unit tests.
 * The fold that reads the stash is networkInjectD2CarryPrior (BorderPrior.cpp).
 *
 * networkRetroUraBookkeeping and networkStashCarry open with an alias block (decomposition
 * INC-4) that binds the driver's former local names to members of the context, so the
 * relocated code keeps those names.
 *
 * One of the seven files that define the network run context's functions: card NOM3 step
 * HV0 split them out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/network/state/RunContext.hpp.
 */

#include "network/state/RunContext.hpp"
#include "network/driver/Epoch.hpp"   // EpochEvents

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat, constellationKey
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "network/state/CarryDisposition.hpp" // carryColumnPresent (card PRM phase 1b)
#include "network/solve/Sbasis.hpp"          // NetworkSbasis::Plan
#include "filter/StateKeySchema.hpp"         // StateKeys::parseSatPhaseBiasKey / makeAmbiguityKey
#include "math/TimeUtils.hpp"                // TimeUtils::continuousGpsSeconds

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

namespace GnssProcessor {

void networkRetroUraBookkeeping(NetworkRunContext& ctx,
                                const std::set<std::string>& clock_refloat_sats,
                                const std::set<std::string>& retro_flag_sats,
                                bool retro_feed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const bool   ura_retro = ctx.knobs.ura_retro;

    // After all post-emit QC tiers, record the flagged satellites for the next emit's
    // retro-bump (per-satellite disc bump) and post-refloat URA inflation horizon (B1).
    // The flag set is the union of clock_refloat_sats (clock model / clock monitor, plus
    // the coupled RAIM tiers) and retro_flag_sats (the tier-independent P0-2
    // accumulator), so the retro-bump covers every QC tier regardless of the
    // clock_monitor_couple_raim opt-in.
    if (retro_feed) {
        std::set<std::string> flagged_now;
        for (const std::string& sat : clock_refloat_sats)
            if (!sat.empty()) flagged_now.insert(sat);
        for (const std::string& sat : retro_flag_sats)
            if (!sat.empty()) flagged_now.insert(sat);

        if (ura_refloat_inflate > 1.0 && pl_warmup > 0)
            for (const std::string& sat : flagged_now) net::putSat(ctx.entities, sat, &net::SatelliteState::ura_refloat_horizon) = pl_warmup;

        // The retro-bump is edge-triggered (P1-3): queue only a satellite that just
        // became flagged (not already in retro_active), so a satellite that stays
        // flagged through several recovery epochs is reset once at the consumer, not at
        // every emit.  A satellite that clears drops out of retro_active, so a later
        // re-flag re-arms the bump.  ura_retro_sats is consumed at the next emit.
        if (ura_retro) {
            for (const std::string& sat : flagged_now)
                if (!net::hasSat(ctx.entities.sats, sat, &net::SatelliteState::retro_active)) net::markSat(ctx.entities, sat, net::Disc::UraRetro);
            net::assignSatFlags(ctx.entities, &net::SatelliteState::retro_active, flagged_now);
        }

        // Age out the post-refloat inflation horizon one epoch at a time.
        if (ura_refloat_inflate > 1.0)
            net::eraseIf(ctx.entities.sats, &net::SatelliteState::ura_refloat_horizon,
                         [](const SatId&, int& left) { return --left <= 0; });
    }
}

void networkStashCarry(NetworkRunContext& ctx, const GnssConfig& config_,
                       NetworkRunTraces& traces, const GnssTime& t,
                       const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                       const NetworkSbasis::Plan& plan,
                       const SphericalHarmonicIonosphere* sh_model,
                       const SphericalHarmonicIonosphere* sh_model2,
                       const std::set<std::string>& clock_refloat_sats,
                       const std::vector<std::string>& registered_clock_sats,
                       const std::set<std::string>& shared_phase_keys,
                       const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                       const EpochEvents& events,
                       const std::shared_ptr<spdlog::logger>& log) {
    // Card PRM phase 2: the stash re-expresses the carry in the solve's datum, so a transform
    // the handover preview applied before PASS B is no longer a pending record (empty with
    // network.handover_stransform_before_rows off).
    ctx.carry.sts_carry_pivot.clear();
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    net::ConstellationTable& cons_rows = ctx.entities.constellations;
    const auto& this_epoch_cb_ifpin = events.code_datum.this_epoch_cb_ifpin;
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;
    const bool   carry_cpl_tu = ctx.knobs.carry_cpl_tu;
    const bool   clk_model_on = ctx.knobs.clk_model_on;
    const bool   carry_invariant = ctx.knobs.carry_invariant;
    const bool   cb_time_linked = ctx.knobs.cb_time_linked;
    const bool   fullcov_on = ctx.knobs.fullcov_on;

    // --- Stash of the D2 carry for the next epoch ------------------------
    // READMIT (E2): keep the outgoing stash, so departures can be parked after the
    // rebuild.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    std::vector<CarryPrior> old_carry;
    if (readmit_h > 0.0) old_carry = prev_carry;
    prev_carry.clear();
    prev_carry_cov.clear();   // rebuilt below when the dense carry (C1) is on
    std::vector<Eigen::Index> fc_cols;  // shared-border column per dense row (C1)
    const Eigen::MatrixXd& cov = est.sharedCovariance();
    std::set<Eigen::Index> pinned_cols;
    for (const auto& p : plan.shared_datum) pinned_cols.insert(p.column);
    auto stash = [&](SharedParamKind kind, const std::string& sat,
                     const std::string& signal, Eigen::Index col, bool inflate) {
        // Skip the datum-pinned columns.  Their solve covariance is 1/pin_scale
        // (not ~0), but the explicit datum re-pins them every epoch (its
        // Dirichlet elimination overrides any prior on them), and carrying one
        // forward would wrongly anchor a retired pivot to its old datum value
        // after a handover.
        if (pinned_cols.count(col)) return;
        const double var = cov(col, col);
        if (!(var > 0.0)) return;
        CarryPrior cp;
        cp.kind = kind; cp.sat = sat; cp.signal = signal;
        cp.value = x_s(col); cp.var = var; cp.inflate_q = inflate;
        prev_carry.push_back(cp);
        if (fullcov_on) {   // register this entry's dense row, same skip rules (C1)
            prev_carry_cov.row.emplace(sharedCarryKey(kind, sat, signal),
                                       fc_cols.size());
            fc_cols.push_back(col);
        }
    };
    // A satellite flagged by the clock monitor (or, opt-in, by the within-epoch / post-fit
    // RAIM) has its carried shared state reinitialized rather than frozen.  Two modes: with
    // clk_mon_reinit_sig > 0 (default) it is still stashed but marked inflate_q, so next
    // epoch folds it with the loose reinit variance (a weak anchor that lets a frozen clock
    // move back toward truth while limiting the datum drift a hard refloat causes); with
    // clk_mon_reinit_sig ≤ 0 it is withheld entirely (hard skip-stash refloat, no prior).
    // clock_refloat_sats is empty unless network.clock_monitor is on, so the stash is
    // byte-identical when the monitor is off (nothing flagged → inflate=false → the
    // ordinary stiff-carry stash).
    const bool full_refloat = !(clk_mon_reinit_sig > 0.0);   // reinit_sigma ≤ 0 → hard skip-stash
    // Carry this stash epoch's clock-monitor flags across to the next inject (hard-refloat
    // mode skip-stashes the flagged satellite entirely, so the flag cannot travel on
    // prev_carry.inflate_q).  Gated, so off runs are unchanged.  Card 1g reads the same
    // history with κ off, so the mirror follows the wider predicate; otherwise arming 1g
    // alone would leave the clock-QC half of the feed empty, and the widening would cover
    // only the reref sources without any indication.
    if (carry_cpl_tu || ctx.knobs.carry_cpl_reinit)
        net::assignSatFlags(ctx.entities, &net::SatelliteState::prev_clock_refloat, clock_refloat_sats);
    // The satellite clock is stashed to prev_carry only in scalar-carry mode.  Under
    // network.clock_model the clock is carried by clock_osc and is not stashed here; the
    // two-state model is its only cross-epoch memory.
    if (!clk_model_on)
        for (const std::string& sat : registered_clock_sats) {
            const bool flagged = clock_refloat_sats.count(sat) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
        }
    // eq34 reattach hygiene (INC-2): refresh the bridge's value source from this kept
    // epoch's solved shared phase biases.  Rebuilt wholesale in lock-step with the D2
    // carry (a departed key drops out, so a bridge cannot be opened on a level from before
    // an absence), and harvested here, at the stash, which every whole-epoch withhold path
    // skips with `continue`, so a withheld epoch's transient solve never becomes a held
    // level.  Not read from prev_carry, on purpose: under carry_invariant the shared
    // PhaseBias carry is not stashed at all, and that is the configuration the bridge
    // targets.  The S-basis completion (network.sbasis_complete, SBC) reuses this harvest
    // for the same reason: its continuity targets need the last solved phase-bias levels
    // on the carry_invariant configurations.  Empty and untouched when neither consumer
    // is armed.
    const bool harvest_pb_levels =
        ctx.knobs.pin_bridge_on || ctx.knobs.sbasis_complete_on;
    if (harvest_pb_levels) net::dropAll(ctx.entities.sat_signals, &net::SatSignalState::pb_level_prev);
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Datum-station slip re-reference (§9.7.10,
        // network.cycle_slip.datum_station_detect): a slip at the gauge-pinned datum
        // receiver moved the level that every phase bias of this satellite references.
        // Skip the stash entirely for one kept epoch, before the pb_level_prev harvest (a
        // bridge must not re-open on the stale level) and before the phase-bias stash,
        // so the next solve re-seeds the bias at the new pivot
        // level, in lock-step with the network-wide ambiguity reseed that the slip drain
        // defers until this skip has been consumed.  The set is empty unless the toggle
        // is armed ⇒ byte-identical.
        if (net::hasSat(ctx.entities.sats, parts->satellite, &net::SatelliteState::datum_reref_pb)) continue;
        if (harvest_pb_levels) {
            // Guarded: this runs before the carry_invariant skip below, so it is the one
            // site that queries a phase-bias column for every shared key on a path that
            // otherwise would not.  A key without a column is not harvested (a bridge
            // then cannot open on it); the stash does not throw.
            try {
                const double v =
                    x_s(est.phaseBiasColumn(parts->satellite, parts->signal));
                if (std::isfinite(v))
                    net::putSig(ctx.entities, StateKeys::makeAmbiguityKey(parts->satellite, parts->signal),
                                &net::SatSignalState::pb_level_prev) = v;
            } catch (...) {}
        }
        // The DD invariant carry is the phase time-link, so the S-dependent shared
        // PhaseBias carry is not stashed as well (it would double-count in next epoch's
        // D2 / dense fold).  Clock / code / SH / ZWD carries are unaffected.
        if (carry_invariant) continue;
        const bool flagged = clock_refloat_sats.count(parts->satellite) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col =
            est.phaseBiasColumn(parts->satellite, parts->signal);
        stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal, col, flagged);
    }
    // The datum re-reference skip-stash queue (§9.7.10) is consumed exactly once per
    // kept epoch (a withheld epoch never reaches this stash, so the queue, and the
    // deferred ambiguity reseed keyed on its consumption, survive the withhold intact).
    // Cleared wholesale: a queued satellite with no phase-bias key this epoch had nothing
    // stashed to skip, and prev_carry is rebuilt from scratch above, so no stale carry is
    // left to guard against.
    if (net::present(ctx.entities.sats, &net::SatelliteState::datum_reref_pb) > 0)
        net::assignSatFlags(ctx.entities, &net::SatelliteState::datum_reref_pb, {});
    // Refresh the time-linked previous-value map from this kept epoch's solved code
    // biases.  Cleared and rebuilt (a departed satellite drops out, so its return
    // re-seeds that satellite alone) in lock-step with the D2 carry, with the same
    // disposition rules (a hard-refloated satellite is skipped from both, so it re-seeds
    // next epoch, with its re-reference signalled as a disc).  Written to the snapshot
    // only under network.state_snapshot_datum_capture (RS-3).  Empty and untouched when
    // off.
    if (cb_time_linked) {
        net::dropAll(cons_rows, &net::ConstellationState::channels,
                     &net::ChannelState::code_bias_prev);
        // Advance the IF-pin comparison baseline to this kept epoch's pairs (a pair
        // recorded on an epoch that was then withheld never reaches here, so the
        // baseline tracks emitted / kept datum realizations only).
        net::assignSat(ctx.entities, &net::SatelliteState::code_bias_ifpin_pair, this_epoch_cb_ifpin);
    }
    for (const auto& kv : registered_code_keys) {  // satellite code bias, OSB (W6)
        const bool flagged = clock_refloat_sats.count(kv.first) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col = est.codeBiasColumn(kv.first, kv.second);
        // CB-excursion guard (SPEC_day129_frame_guards phase 2.1): a network-wide
        // common-mode clock excursion this epoch marks the affected satellite's code-bias
        // carry for reinit, and only the code-bias carry.  The clock and phase-bias
        // carries are left untouched on purpose: the clock is soft (q = 0.3 m²/ep; it
        // recovers on its own in 2–4 epochs), and the phase biases did not move
        // measurably (≤ 4 mm at the DOY-129 event).  The near-static code bias is the
        // state that ratchets, so it is the state the guard resets.  The set is empty
        // unless network.cb_excursion_reinit is on ⇒ byte-identical when off.
        const bool cb_excursion = events.qc.cb_excursion_sats.count(kv.first) > 0;
        stash(SharedParamKind::CodeBias, kv.first, kv.second, col,
              flagged || cb_excursion);
        if (cb_time_linked) {
            const double v = x_s(col);
            if (std::isfinite(v)) {
                net::put(cons_rows, systemOfSat(kv.first), &net::ConstellationState::channels,
                         kv.second, &net::ChannelState::code_bias_prev)[kv.first] = v;
                net::markSat(ctx.entities, kv.first, &net::SatelliteState::code_bias_seen);
            }
        }
    }
    // Dense carry (C1): stash the dense covariance block over the clock/phase/code
    // entries just recorded (the SH block below keeps its own diagonal stash; its carry
    // is the separate fold before the gate).  One O(k²) copy from the solve's
    // posterior.  A single non-finite entry clears the whole block, and the next fold
    // degrades per key to the diagonal path instead of corrupting the next epoch's
    // normals (the crash mode the SH guard below screens per entry).
    if (fullcov_on && !fc_cols.empty()) {
        const Eigen::Index kfc = static_cast<Eigen::Index>(fc_cols.size());
        prev_carry_cov.P.resize(kfc, kfc);
        for (Eigen::Index a = 0; a < kfc; ++a)
            for (Eigen::Index b = 0; b < kfc; ++b)
                prev_carry_cov.P(a, b) = cov(fc_cols[static_cast<std::size_t>(a)],
                                             fc_cols[static_cast<std::size_t>(b)]);
        if (!prev_carry_cov.P.allFinite()) {
            log->warn("Network (C1): non-finite entries in the stashed shared-"
                      "covariance block ({} keys) — dense carry cleared; the next "
                      "epoch folds the diagonal carry.", fc_cols.size());
            prev_carry_cov.clear();
        }
    }
    // Card CTU: stamp the block with this epoch's content time (a coupling row is exact
    // only if it was harvested now, against this same posterior) and record which clock
    // the datum pinned (the pivot takes each exact row's clock-family deficit next epoch).
    if (ctx.knobs.carry_cpl_exact) {
        ctx.carry.cxu_stash_s     = TimeUtils::continuousGpsSeconds(t);
        ctx.carry.cxu_stash_valid = prev_carry_cov.P.rows() > 0;
        net::dropAll(ctx.entities.constellations, &net::ConstellationState::cxu_pivot);
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            Eigen::Index col = -1;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (pinned_cols.count(col) && col >= 0 && col < x_s.size())
                net::put(ctx.entities.constellations, systemOfSat(sat),
                         &net::ConstellationState::cxu_pivot) = {sat, x_s(col)};
        }
    }
    // Card PFQ: the clock pivot this stash was pinned on, the frame of the carried clocks
    // the next epoch's fold applies the pivot-frame process noise in.
    if (ctx.knobs.pfq_on) {
        net::dropAll(ctx.entities.constellations, &net::ConstellationState::pfq_pivot);
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            Eigen::Index col = -1;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (col >= 0 && pinned_cols.count(col))
                net::put(ctx.entities.constellations, systemOfSat(sat),
                         &net::ConstellationState::pfq_pivot) = sat;
        }
    }
    if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coefficients
        for (const ShLayer& lay :
             shLayers(config_.getGlobalVtecShConfig(), sh_model, sh_model2)) {
            for (const auto& c : lay.model->coeffs()) {
                const Eigen::Index col =
                    est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer);
                if (pinned_cols.count(col)) continue;   // SH cols are never datum-pinned (safety)
                const double var = cov(col, col);
                const double val = x_s(col);
                // A non-finite value or variance is not carried forward: it would
                // corrupt the next epoch's normals and crash the solve.  The
                // mean-reverting climatology prior re-seeds the coefficient.
                if (!(var > 0.0) || !std::isfinite(val) || !std::isfinite(var)) continue;
                CarryPrior cp;
                cp.kind = SharedParamKind::VtecSh;
                cp.sh_n = c.n; cp.sh_m = c.m; cp.sh_trig = c.trig;
                cp.sh_layer = lay.layer;
                cp.value = val; cp.var = var;
                prev_carry.push_back(cp);
            }
        }
    }

    // ---- READMIT continuity (E2 seed-first re-admission and the block re-coupling edge) ----
    // (SPEC_synthetic_minimum_network §18.4; SPEC_amb_iono_split_overcount §7 FIRST.)  Runs
    // after the wholesale rebuild above, so "absent from the new stash" is exact, and on
    // kept epochs only: a withheld epoch never reaches here, which is why the detector
    // needs >= kReadmitMinGapEp loop epochs of absence rather than one.
    if (readmit_h > 0.0 || ctx.knobs.readmit_marg_ep > 0) {
        const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        // (1) The re-admission detector and the coupling marginalization window (the
        //     block's decoupling edge, standing invariant #7): a satellite whose clock
        //     column is registered now but was last stashed >= kReadmitMinGapEp loop
        //     epochs ago.
        for (const std::string& sat : registered_clock_sats) {
            long long& last = net::putSat(ctx.entities, sat, &net::SatelliteState::readmit_last_seen_ep);   // 0 on first sight
            const long long gap_ep = (last > 0) ? ep - last : 0;
            const bool readmitted = last > 0 && gap_ep >= kReadmitMinGapEp;
            last = ep;
            if (!readmitted) continue;
            ++ctx.counters.n_readmit_events;
            std::string marg;
            if (ctx.knobs.readmit_marg_ep > 0) {
                net::putSat(ctx.entities, sat, &net::SatelliteState::readmit_marg) = ep + ctx.knobs.readmit_marg_ep;
                ++ctx.counters.n_readmit_marg_windows;
                marg = fmt::format(";marg_until_ep={}", ep + ctx.knobs.readmit_marg_ep);
            }
            traces.emitEvent(t, "READMIT", "readmit", sat, "", static_cast<double>(gap_ep),
                             static_cast<double>(kReadmitMinGapEp), "readmit",
                             fmt::format("gap_ep={}{}", gap_ep, marg));
        }
        // Expired windows: the block's re-coupling edge (the returned satellite's
        // columns fold as present terms again), traced per satellite.
        net::eraseIf(ctx.entities.sats, &net::SatelliteState::readmit_marg, [&](const SatId& id, long long until) {
            if (ep < until) return false;
            traces.emitEvent(t, "READMIT", "marg_expired", net::satKey(id), "",
                             static_cast<double>(until), std::nullopt, "recouple",
                             "coupling terms on this satellite fold as PRESENT again");
            return true;
        });
        // (2) Park departures; retire consumed or expired parks (the seed knob only).
        if (readmit_h > 0.0) {
            const double now_s = ctx.carry.readmit_now_s;
            std::set<std::tuple<int, std::string, std::string>> live;
            for (const CarryPrior& cp : prev_carry)
                live.emplace(static_cast<int>(cp.kind), cp.sat, cp.signal);
            std::map<std::string, int> parked_now;
            for (const CarryPrior& oc : old_carry) {
                // The phase-bias frame is re-realized at the cold re-pin (the leveling
                // carries do not survive a 55 min absence at the 120 s leveling gap), and
                // a stiff PB seed would conflict with that pin, so only Clock and CodeBias
                // park.
                if (oc.kind != SharedParamKind::Clock
                    && !(oc.kind == SharedParamKind::CodeBias && ctx.knobs.readmit_seed_cb))
                    continue;
                if (live.count({static_cast<int>(oc.kind), oc.sat, oc.signal})) continue;
                const bool present = carryColumnPresent(est, oc);
                if (oc.t_park_s > 0.0) {
                    // Already parked.  Column back but not re-stashed (skip-stash,
                    // pinned, non-finite) ⇒ the memory is dropped with the skip; past the
                    // horizon ⇒ drop.
                    // Card PRM phase 3a: each drop counted per kind of the parked entry
                    // (a Clock, or a CodeBias under readmit_seed_cb: the filter above).
                    if (present) {
                        ++ctx.counters.n_readmit_dropped_stale;
                        prmRespond(ctx.counters.prm_resp,
                                   oc.kind == SharedParamKind::Clock
                                       ? ResponseSite::ClkParkDiscard
                                       : ResponseSite::CbParkDiscard);
                        continue;
                    }
                    if (now_s - oc.t_park_s > readmit_h) {
                        ++ctx.counters.n_readmit_expired;
                        prmRespond(ctx.counters.prm_resp,
                                   oc.kind == SharedParamKind::Clock
                                       ? ResponseSite::ClkParkExpire
                                       : ResponseSite::CbParkExpire);
                        continue;
                    }
                    prev_carry.push_back(oc);
                    continue;
                }
                // Live last epoch, absent now: a departure only if the column itself
                // left the layout (a skip-stash / pinned / non-finite key is not a
                // departure).
                if (present) continue;
                CarryPrior cp = oc;
                cp.t_park_s  = (ctx.carry.readmit_prev_stash_s > 0.0)
                                   ? ctx.carry.readmit_prev_stash_s : now_s;
                cp.inflate_q = false;
                prev_carry.push_back(std::move(cp));
                ++ctx.counters.n_readmit_parked;
                prmRespond(ctx.counters.prm_resp,   // card PRM phase 3a, per kind
                           oc.kind == SharedParamKind::Clock ? ResponseSite::ClkPark
                                                             : ResponseSite::CbPark);
                ++parked_now[oc.sat];
            }
            for (const auto& [sat, n] : parked_now) {
                traces.emitEvent(t, "READMIT", "park", sat, "", static_cast<double>(n),
                                 readmit_h, "park",
                                 "D2 clock + code-bias carry parked at departure");
                // The READMIT writer (card SID phase 4a, S1).  The park is a shared
                // parameter's departure, so it names a satellite and never a station:
                // the expansion writes the bit on every station's arcs of that satellite
                // that the admitted lifecycle still considers alive.  This seam runs
                // after the roster's audit pass, so the record reaches the fold one epoch
                // later through @ref kArcReasonLateMask, the earliest epoch at which a gap
                // it can explain can open (the satellite has already departed).
                arcReasonExpand(ctx.reasons, ctx.arc_ledger, -1, sat, "",
                                kArcReasonReadmit);
            }
            // (3) Seed-vs-solve check: mark-if-moved telemetry for every seeded kind (the
            //     clock's authoritative mark is the Q3 guard's own trip, counted separately).
            for (const auto& sd : events.solve.readmit_seed_check) {
                Eigen::Index col = -1;
                try {
                    col = (sd.kind == SharedParamKind::Clock)
                              ? est.clockColumn(sd.sat)
                              : est.codeBiasColumn(sd.sat, sd.signal);
                } catch (...) { continue; }
                if (col < 0 || col >= x_s.size() || col >= cov.rows()) continue;
                const double e = x_s(col) - sd.value;
                const double S = std::max(1e-12, sd.var_eff - cov(col, col));
                const double w = std::abs(e) / std::sqrt(S);
                if (!std::isfinite(w)) continue;
                ++ctx.counters.n_readmit_seed_check;
                const bool out = w > kReadmitSeedK;
                if (out) ++ctx.counters.n_readmit_seed_outlier;
                traces.emitEvent(t, "READMIT", out ? "seed_moved" : "seed_held", sd.sat,
                                 sd.signal, w, kReadmitSeedK, out ? "telemetry" : "ok",
                                 fmt::format("kind={};e={:.4f};sqrtS={:.4f}",
                                             sd.kind == SharedParamKind::Clock ? "clock"
                                                                               : "code_bias",
                                             e, std::sqrt(S)));
            }
            ctx.carry.readmit_prev_stash_s = now_s;
        }
    }
}

} // namespace GnssProcessor
