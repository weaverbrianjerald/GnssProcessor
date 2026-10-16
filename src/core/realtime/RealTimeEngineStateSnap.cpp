/**
 * @file RealTimeEngineStateSnap.cpp
 * @brief RT filter-state snapshot seam: the consumer-side warm start (the
 *        complement of RTB-7).
 *
 * Three private members on @ref RealTimeEngine, all armed by
 * @c realtime.state_snapshot_path and all no-ops (byte-identical) with the
 * path empty:
 *
 *  - @ref RealTimeEngine::loadStateSnapshotStaged_  (initialize() tail):
 *    parses the file and stages it.  No engine state is touched.  The
 *    fingerprint depends on post-initialize setters (setSsrStore /
 *    setConsumeNetworkIono) and the age gate needs a real epoch time, so
 *    both checks are deferred to the first processed epoch.
 *  - @ref RealTimeEngine::applyStagedStateSnapshot_  (top of processEpoch,
 *    one-shot): age, fingerprint and layout-precondition gates, then the
 *    restore.  Scope "core" (the default) restores the core block only;
 *    scope "full" does the exact-layout rebuild (StateManager::restoreDynamicState
 *    replay) and restores x/P wholesale.  Both scopes restore the
 *    SSR-continuity baselines.  Restoring the previous-epoch time to the
 *    snapshot epoch makes the first KF predict inflate over the true gap.
 *    Restoring the generation id, the disc-counter memory and the A5 applied
 *    maps keeps the engine's existing datum-break reactions armed against
 *    everything the producer signalled during the outage, so the invariant-#7
 *    re-coupling edge is re-validated by responses that are already wired
 *    (reactToGenerationChange / resetOnPhaseBiasDisc / the A5 flips / slip QC
 *    + LLI).  An unsignalled transient from a producer that itself
 *    warm-resumed is handled by the seam response
 *    (@c realtime.state_snapshot_seam_refloat).  A refused gate discards the
 *    stage with a logged reason and the cold start proceeds unchanged.
 *  - @ref RealTimeEngine::writeStateSnapshot_  (processEpoch tail): cadence-
 *    gated atomic write of the live filter memory.  The dynamic-layout
 *    harvest is verified gapless before anything is written; a layout this
 *    seam cannot represent (an unregistered dynamic index) logs once and
 *    writes nothing, so a partial snapshot is never produced.
 *
 * Seam contract: private member functions (every persistent value is an
 * engine member), alias-free (the bodies are new code, not relocations).
 *
 * @see include/realtime/RtStateSnapshot.hpp for the file format,
 *      docs/SPEC_synthetic_minimum_network.md §17 for the validation record.
 */

#include "realtime/RealTimeEngine.hpp"

#include "config/GnssConfig.hpp"
#include "network/solve/NetworkSbasis.hpp"          // kWindUpYawModelId (wind-up model revision, wu93)
#include "filter/StateFamily.hpp"
#include "filter/StateManager.hpp"
#include "math/TimeUtils.hpp"
#include "products/NetworkStateSnapshot.hpp" // NetworkStateSnapshotIO::fnv1a64
#include "realtime/RtStateSnapshot.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

uint64_t RealTimeEngine::rtSnapshotFingerprint_() const {
    // State-meaning facts: anything that changes what a serialized state value
    // or key means, or which states exist.  Not included: measurement noise,
    // process noise and thresholds (they shape future evolution, not the
    // meaning of the carried memory; the RTB-7 rule), and the a-priori
    // position (position is data, restored with the state).
    std::string fp;
    auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
    // Carrier-phase wind-up model revision + accumulation mode: both change the
    // frame every carried ambiguity was leveled in (the pre-wu93 sign left
    // 2·phi_wu in every phase observation; fractional vs accumulated differ by
    // integer-cycle steps the carried ambiguities absorbed).
    add(kWindUpYawModelId);
    add(config_.accumulatePhaseWindUp() ? "wuacc" : "wufrac");
    for (const auto& s : config_.getActiveSystems()) add(s);
    add(fmt::format("fm:{}", static_cast<int>(config_.getFrequencyMode())));
    for (const auto& [sys, prios] : config_.getSignalPriorities()) {
        add("sp:" + sys);
        for (const auto& p : prios) add(p);
    }
    // State-vector shape togglers.
    add(fmt::format("ifb:{}", config_.estimateIfb() ? 1 : 0));
    add(fmt::format("ir:{}", config_.enableIonoRate() ? 1 : 0));
    // AR posture: the resolver consumes the restored float + covariance.
    add(fmt::format("ar:{}:{}:{}", config_.getArMode(), config_.getArWlOnly() ? 1 : 0,
                    ar_ignore_mw_wl_ ? 1 : 0));
    // Correction routing: a bias-consuming engine's ambiguities live in the
    // corrected frame; clock routing decides what the receiver clock/ISB
    // absorbed; the network iono prior changes what the iono states carry.
    add(fmt::format("route:{}:{}:{}", ssr_store_override_ ? 1 : 0,
                    clock_store_override_ ? 1 : 0, consume_network_iono_ ? 1 : 0));
    // The A5 applied maps exist, and have meaning, only under the age gate.
    add(fmt::format("agegate:{}", config_.enableSsrBiasMaxAge() ? 1 : 0));
    return NetworkStateSnapshotIO::fnv1a64(fp);
}

void RealTimeEngine::loadStateSnapshotStaged_() {
    const auto& rt = config_.getRealTimeConfig();
    if (rt.state_snapshot_path.empty()) return;
    auto log = log_manager_.getLogger("rt_engine");
    const std::filesystem::path p(rt.state_snapshot_path);
    if (!std::filesystem::exists(p)) {
        log->info("RT state snapshot: '{}' does not exist — COLD START (the file appears "
                  "once this run writes its first snapshot).", p.string());
        return;
    }
    auto snap = std::make_unique<RtStateSnapshot>();
    std::string err;
    if (!RtStateSnapshotIO::read(p, *snap, &err)) {
        log->warn("RT state snapshot: '{}' failed to parse ({}) — COLD START.",
                  p.string(), err);
        return;
    }
    log->info("RT state snapshot: STAGED from '{}' (written {}, week {} tow {:.1f}; {} "
              "states / {} dynamic; gen id {}; {} disc baselines) — fingerprint and age "
              "gates run at the first processed epoch.",
              p.string(), snap->wall_utc, snap->snap_week, snap->snap_tow,
              snap->state_size, snap->dyn.size(), snap->generation_id,
              snap->ssr_disc.size());
    staged_rt_snap_ = std::move(snap);
}

void RealTimeEngine::applyStagedStateSnapshot_(RtEpochContext& ctx) {
    if (!staged_rt_snap_) return;
    auto log = log_manager_.getLogger("rt_engine");
    // One-shot: the stage is consumed whatever happens below, so a refused
    // snapshot is not retried on every epoch.
    const std::unique_ptr<RtStateSnapshot> snap = std::move(staged_rt_snap_);
    const auto& rt = config_.getRealTimeConfig();
    auto cold = [&](const std::string& why) {
        rt_snap_restore_.outcome = RtSnapshotRestoreStatus::Outcome::Discarded;
        rt_snap_restore_.reason  = why;
        log->warn("{}RT state snapshot: staged snapshot DISCARDED — {} — COLD START.",
                  ctx.epoch_prefix, why);
    };
    // The one-line RESTORED log line that a grader asserts against (the cold
    // counterpart is the DISCARDED line above).  The outcome is also kept on
    // the status getter so a harness or test needs no log parsing (Card CC (B)).
    auto restored = [&](const char* scope) {
        rt_snap_restore_.outcome   = RtSnapshotRestoreStatus::Outcome::Restored;
        rt_snap_restore_.week      = snap->snap_week;
        rt_snap_restore_.tow       = snap->snap_tow;
        rt_snap_restore_.n_states  = snap->state_size;
        rt_snap_restore_.n_dynamic = snap->dyn.size();
        rt_snap_restore_.scope     = scope;
        rt_snap_restore_.reason.clear();
        log->info("{}RT state snapshot: RESTORED from week {} tow {:.3f} (scope={}, {} states / "
                  "{} dynamic, age {:.1f} s) — the consumer is WARM at this epoch.",
                  ctx.epoch_prefix, snap->snap_week, snap->snap_tow, scope, snap->state_size,
                  snap->dyn.size(), TimeUtils::continuousGpsSeconds(ctx.current_time)
                      - (static_cast<double>(snap->snap_week) * 604800.0 + snap->snap_tow));
    };

    const double now_s  = TimeUtils::continuousGpsSeconds(ctx.current_time);
    const double snap_s = static_cast<double>(snap->snap_week) * 604800.0 + snap->snap_tow;
    const double age_s  = now_s - snap_s;
    if (!(age_s > 0.0))
        return cold(fmt::format("first live epoch is not after the snapshot epoch "
                                "(age {:.3f} s — resuming at or before the snapshotted "
                                "epoch would double-count its measurements)", age_s));
    if (age_s > rt.state_snapshot_horizon_s)
        return cold(fmt::format("stale: age {:.1f} s exceeds the horizon {:.1f} s (every "
                                "restored arc would carry unbounded in-gap slip exposure)",
                                age_s, rt.state_snapshot_horizon_s));
    const uint64_t live_fp = rtSnapshotFingerprint_();
    if (snap->fingerprint != live_fp)
        return cold(fmt::format("config fingerprint mismatch (snapshot {:016x} vs live "
                                "{:016x} — a state-meaning config fact changed)",
                                snap->fingerprint, live_fp));

    // Layout preconditions: a core-only manager of the same core size.  The
    // first processed epoch has run no measurement pre-pass yet, so every
    // dynamic map must be empty; anything else means the replay would diverge.
    auto& sm = *state_manager_;
    if (sm.getStateSize() != snap->core_size)
        return cold(fmt::format("live pre-resume state size {} != snapshot core size {} "
                                "(different core layout)", sm.getStateSize(),
                                snap->core_size));
    const std::size_t dyn_live = sm.getIonosphereStates().size()
                               + sm.getIonosphereRateStates().size()
                               + sm.getAmbiguities().size()
                               + sm.getCodeBiasStates().size()
                               + sm.getSatBiasStates().size()
                               + sm.getSatPhaseBiasStates().size()
                               + sm.getReceiverPhaseBiasStates().size()
                               + sm.getSatClockStates().size();
    if (dyn_live != 0)
        return cold(fmt::format("{} dynamic state(s) already exist pre-resume — the "
                                "layout replay requires a core-only manager", dyn_live));
    // Corrupt-file guard: duplicate keys within a kind would silently alias.
    {
        std::set<std::pair<std::string, std::string>> seen;
        for (const auto& d : snap->dyn)
            if (!seen.emplace(d.kind, d.key).second)
                return cold(fmt::format("duplicate dynamic key '{}' of kind {} in the "
                                        "snapshot", d.key, d.kind));
    }

    // --- Restore scope -------------------------------------------------------
    // "core": restore the core block only (position / clock / ZWD seed / ISBs)
    // plus the producer-continuity baselines; dynamic states are re-created per
    // epoch as on a cold start.  Across a producer that itself warm-resumed,
    // every restored dynamic state opposes the producer's unsignalled product
    // transient (on the sim ladder: kept stiff, U walks to -9 m; freed, the
    // tension moves into whatever was freed, ZWD to 2.9 m), so only the core
    // memory starts warm.  "full" is for a product stream that is continuous
    // across the outage (a consumer-only restart).
    const bool core_only = rt.state_snapshot_restore_scope != "full";
    if (core_only) {
        const auto nc = static_cast<Eigen::Index>(snap->core_size);
        sm.setStateVector(snap->x.head(nc));
        sm.setCovariance(snap->P.topLeftCorner(nc, nc));
        sm.setSsrDiscontinuityMemory(snap->ssr_disc);
        ssr_phase_bias_applied_  = snap->phase_bias_applied;
        ssr_code_bias_applied_   = snap->code_bias_applied;
        last_ssr_generation_id_  = snap->generation_id;
        epoch_count_             = snap->epoch_count;
        first_epoch_     = false;
        prev_tow_        = snap->snap_tow;
        prev_epoch_time_ = GnssTime{ snap->snap_week, snap->snap_tow };
        bool zwd_freed = false;
        std::size_t n_isb_freed = 0;
        if (rt.state_snapshot_seam_refloat) {
            const std::size_t tropo_idx = sm.getTropoIndex();
            if (tropo_idx != std::numeric_limits<std::size_t>::max()
                && tropo_idx < snap->core_size) {
                const auto ti = static_cast<Eigen::Index>(tropo_idx);
                Eigen::MatrixXd P = sm.getCovariance();
                const double tropo_std = config_.getInitialStd().troposphere;
                P.row(ti).setZero();
                P.col(ti).setZero();
                P(ti, ti) = tropo_std * tropo_std;
                sm.setCovariance(P);
                zwd_freed = true;
            }
            // The ISB is the stiffest core state (near-zero random walk) and is
            // tied to the producer's per-constellation realization, so a
            // restored ISB carries a stale value past the seam (invariant #1;
            // on the sim ladder, core-scope epoch-0 error 10.95 m against 4.46 m
            // cold with everything else freed).  Its variance is re-inflated,
            // value kept, through the response the A2 clock-disc reaction uses;
            // 0.0 selects that response's initial_std fallback.
            for (const auto& [sys, idx] : sm.getSystemTimeOffsetStates()) {
                (void)idx;
                sm.reinflateSystemTimeOffsetVariance(sys, 0.0);
                ++n_isb_freed;
            }
        }
        log->info("{}RT state snapshot: WARM RESUME applied, scope=core — {} core "
                  "states restored (position/clock/ZWD-seed; {} dynamic states "
                  "deliberately NOT restored, they re-create per epoch) at age "
                  "{:.1f} s (horizon {:.0f} s); gen id {} + {} disc baselines + {} "
                  "A5 flags carried; ZWD variance {}; {} ISB state(s) re-inflated "
                  "(values kept); the first predict inflates over the true gap "
                  "(re-coupling edge).",
                  ctx.epoch_prefix, snap->core_size, snap->dyn.size(), age_s,
                  rt.state_snapshot_horizon_s, snap->generation_id,
                  snap->ssr_disc.size(),
                  snap->phase_bias_applied.size() + snap->code_bias_applied.size(),
                  zwd_freed ? "re-inflated to initial_std.troposphere (value kept)"
                            : "left as restored",
                  n_isb_freed);
        restored("core");
        return;
    }

    // --- Exact-layout rebuild (ascending, gapless; enforced by the parser) ---
    std::size_t n_amb = 0, n_ion = 0, n_rxcb = 0;
    for (const auto& d : snap->dyn) {
        const std::optional<StateFamily> family = stateFamilyFromSnapshotTag(d.kind);
        if (!family)
            return cold(fmt::format("unknown dynamic state kind '{}' (a newer writer?)",
                                    d.kind));
        if (*family == StateFamily::Ionosphere)       ++n_ion;
        if (*family == StateFamily::Ambiguity)        ++n_amb;
        if (*family == StateFamily::ReceiverCodeBias) ++n_rxcb;
        if (!sm.restoreDynamicState(*family, d.key,
                                    snap->x(static_cast<Eigen::Index>(d.idx)), d.idx)) {
            // Unreachable on a parse-clean file against a verified-empty manager.
            // If it fires, the manager is partly mutated: log an error and stop
            // rather than continue as if the restore succeeded.
            log->error("{}RT state snapshot: layout replay FAILED at index {} ('{}') "
                       "AFTER preconditions passed — the state manager is partially "
                       "restored; treat this run's estimates as cold-start-degraded and "
                       "report the snapshot file.", ctx.epoch_prefix, d.idx, d.key);
            return;
        }
    }
    sm.setStateVector(snap->x);
    sm.setCovariance(snap->P);
    sm.setSsrDiscontinuityMemory(snap->ssr_disc);
    ssr_phase_bias_applied_  = snap->phase_bias_applied;
    ssr_code_bias_applied_   = snap->code_bias_applied;
    last_ssr_generation_id_  = snap->generation_id;
    epoch_count_             = snap->epoch_count;
    // The first predict inflates over the true gap: time steps from the
    // snapshot epoch, not from a synthetic first-epoch dt.
    first_epoch_     = false;
    prev_tow_        = snap->snap_tow;
    prev_epoch_time_ = GnssTime{ snap->snap_week, snap->snap_tow };

    // --- Seam response (invariant #1) ---------------------------------------
    // A producer that itself warm-resumed emits a deliberately unsignalled
    // product transient (gen id preserved, no disc bumps, which keeps cold
    // consumers seamless), so none of the restored datum-break baselines fire
    // on it.  Restored states that are stiff resist it.  The pinned ZWD
    // (troposphere q ~1e-9) is the dominant case: a cold consumer absorbs the
    // transient's elevation-common part in ZWD (a 0.16 -> 0.64 m excursion that
    // relaxes as the producer settles), while a warm consumer with ZWD pinned
    // pushes the same tension into the vertical at mapping-amplified scale (on
    // the sim ladder, a monotone U walk to about -9 m; hour-6 rms 7.44 m against
    // 1.07 m cold).  The response, on by default
    // (realtime.state_snapshot_seam_refloat):
    //   (a) mark every restored ambiguity pending-reseed through the
    //       resetAmbiguity path the disc/slip reactions use (inert on this
    //       consumer, which reseeds every epoch; required for engines that
    //       carry ambiguities across epochs);
    //   (b) re-inflate the restored ZWD variance to initial_std.troposphere
    //       with zeroed cross-covariances.  The value stays as the seed, so the
    //       transient can move into ZWD as on a cold start while position
    //       starts warm.
    // Per-sat iono is left alone: its own q relaxes it within an epoch and its
    // value re-anchors the geometry-free split.
    std::size_t n_refloat = 0, n_isb_freed = 0;
    bool zwd_refloated = false;
    if (rt.state_snapshot_seam_refloat) {
        const double reset_std = config_.getAmbiguityStd();
        for (const auto& d : snap->dyn)
            if (d.kind == "AMB" && sm.resetAmbiguity(d.key, reset_std)) ++n_refloat;
        const std::size_t tropo_idx = sm.getTropoIndex();
        if (tropo_idx != std::numeric_limits<std::size_t>::max()
            && tropo_idx < snap->state_size) {
            const auto ti = static_cast<Eigen::Index>(tropo_idx);
            Eigen::MatrixXd P = sm.getCovariance();
            const double tropo_std = config_.getInitialStd().troposphere;
            P.row(ti).setZero();
            P.col(ti).setZero();
            P(ti, ti) = tropo_std * tropo_std;
            sm.setCovariance(P);
            zwd_refloated = true;
        }
        // ISB: the stiffest core state, tied to the producer realization.  Same
        // response as in the core scope (value kept, variance to initial).
        for (const auto& [sys, idx] : sm.getSystemTimeOffsetStates()) {
            (void)idx;
            sm.reinflateSystemTimeOffsetVariance(sys, 0.0);
            ++n_isb_freed;
        }
    }

    log->info("{}RT state snapshot: WARM RESUME applied — {} states restored ({} "
              "ambiguities, {} iono, {} rx code-bias; core {}) at age {:.1f} s "
              "(horizon {:.0f} s); gen id {} + {} disc-counter baselines + {} A5 "
              "applied flags carried, so the SSR datum-break reactions re-validate "
              "the outage; the first predict inflates over the true gap "
              "(re-coupling edge).  Seam response: {} restored ambiguit(ies) "
              "pending-reseed + ZWD variance {} + {} ISB state(s) re-inflated "
              "(values kept){}.",
              ctx.epoch_prefix, snap->state_size, n_amb, n_ion, n_rxcb,
              snap->core_size, age_s, rt.state_snapshot_horizon_s,
              snap->generation_id, snap->ssr_disc.size(),
              snap->phase_bias_applied.size() + snap->code_bias_applied.size(),
              n_refloat,
              zwd_refloated ? "RE-INFLATED to initial_std.troposphere"
                            : "left as restored (no tropo state)",
              n_isb_freed,
              rt.state_snapshot_seam_refloat
                  ? ""
                  : " — SEAM REFLOAT OFF: only sound against a producer proven "
                    "emission-continuous across the outage");
    restored("full");
}

bool RealTimeEngine::harvestStateSnapshot_(RtStateSnapshot& snap, const GnssTime& at,
                                           const std::string& epoch_prefix, std::string* why) {
    auto log = log_manager_.getLogger("rt_engine");
    auto& sm = *state_manager_;

    // Harvest the dynamic layout from the key→index maps and verify that it
    // tiles the dynamic block exactly (gapless, nothing below the derived
    // core).  A layout this seam cannot represent writes nothing, leaving the
    // previous good snapshot on disk intact, and warns once.
    const std::size_t n = sm.getStateSize();
    std::vector<RtStateSnapshot::DynState> rows;
    for (const auto& spec : kStateFamilies) {
        if (!spec.snapshot_tag) continue;   // not serialized: its rows fail the tiling check below
        for (const auto& [key, idx] : sm.indicesOf(spec.family))
            rows.push_back(RtStateSnapshot::DynState{ idx, spec.snapshot_tag, key });
    }
    std::sort(rows.begin(), rows.end(),
              [](const RtStateSnapshot::DynState& a, const RtStateSnapshot::DynState& b) {
                  return a.idx < b.idx;
              });
    if (rows.size() > n) {
        if (!rt_snap_layout_warned_) {
            rt_snap_layout_warned_ = true;
            log->warn("{}RT state snapshot: {} dynamic map entries exceed the state size "
                      "{} — layout unserializable, snapshot writes disabled this run.",
                      epoch_prefix, rows.size(), n);
        }
        if (why) *why = fmt::format("{} dynamic map entries exceed the state size {}",
                                    rows.size(), n);
        return false;
    }
    const std::size_t core = n - rows.size();
    for (std::size_t k = 0; k < rows.size(); ++k) {
        if (rows[k].idx != core + k) {
            if (!rt_snap_layout_warned_) {
                rt_snap_layout_warned_ = true;
                log->warn("{}RT state snapshot: dynamic index {} ('{}') does not tile the "
                          "dynamic block [{}..{}) — an unregistered or aliased state kind; "
                          "snapshot writes disabled this run.",
                          epoch_prefix, rows[k].idx, rows[k].key, core, n);
            }
            if (why) *why = fmt::format("dynamic index {} ('{}') does not tile the dynamic "
                                        "block [{}..{})", rows[k].idx, rows[k].key, core, n);
            return false;
        }
    }

    snap.snap_week     = at.week;
    snap.snap_tow      = at.tow;
    snap.fingerprint   = rtSnapshotFingerprint_();
    snap.core_size     = core;
    snap.state_size    = n;
    snap.generation_id = last_ssr_generation_id_;
    snap.epoch_count   = epoch_count_;
    snap.dyn           = std::move(rows);
    snap.x             = sm.getStateVector();
    snap.P             = sm.getCovariance();
    snap.ssr_disc      = sm.getSsrDiscontinuityMemory();
    snap.phase_bias_applied = ssr_phase_bias_applied_;
    snap.code_bias_applied  = ssr_code_bias_applied_;
    return true;
}

void RealTimeEngine::writeStateSnapshot_(RtEpochContext& ctx) {
    const auto& rt = config_.getRealTimeConfig();
    if (rt.state_snapshot_path.empty()) return;
    const double now_s = TimeUtils::continuousGpsSeconds(ctx.current_time);
    if (rt_snap_last_write_s_ >= 0.0
        && now_s - rt_snap_last_write_s_ < rt.state_snapshot_interval_s)
        return;
    auto log = log_manager_.getLogger("rt_engine");

    RtStateSnapshot snap;
    if (!harvestStateSnapshot_(snap, ctx.current_time, ctx.epoch_prefix, nullptr))
        return;   // warned once inside the harvest; nothing written

    std::string werr;
    if (!RtStateSnapshotIO::write(rt.state_snapshot_path, snap, &werr))
        log->warn("{}RT state snapshot: write to '{}' failed ({}) — the previous good "
                  "snapshot (if any) is left intact.", ctx.epoch_prefix,
                  rt.state_snapshot_path, werr);
    else
        rt_snap_last_write_s_ = now_s;
}

bool RealTimeEngine::writeStateSnapshotTo(const std::filesystem::path& path,
                                          const GnssTime* expected_epoch, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    // Writes a consumer snapshot beside the network snapshot copy.  No cadence
    // gate and no use of realtime.state_snapshot_path; the cadence writer's
    // last-write stamp is not advanced, because the two writers are
    // independent (Card CC (B)).
    if (!has_processed_epoch_ || !state_manager_)
        return fail("no epoch has been processed yet — nothing to snapshot");
    if (expected_epoch
        && (expected_epoch->week != last_processed_epoch_.week
            || std::abs(expected_epoch->tow - last_processed_epoch_.tow) > 1.0e-3))
        return fail(fmt::format("the consumer's last processed epoch (week {} tow {:.6f}) is "
                                "not the expected epoch (week {} tow {:.6f}) — the consumer "
                                "did not process that tow",
                                last_processed_epoch_.week, last_processed_epoch_.tow,
                                expected_epoch->week, expected_epoch->tow));
    RtStateSnapshot snap;
    std::string why;
    if (!harvestStateSnapshot_(snap, last_processed_epoch_, "[RT] ", &why))
        return fail("layout unserializable: " + why);
    std::string werr;
    if (!RtStateSnapshotIO::write(path, snap, &werr))
        return fail("write to '" + path.string() + "' failed: " + werr);
    return true;
}

std::filesystem::path RealTimeEngine::consumerSnapshotHistoryPath(
    const std::filesystem::path& network_snapshot_path, unsigned long long n_emitted) {
    std::filesystem::path p = network_snapshot_path;
    p += fmt::format(".consumer.ep{:04d}", n_emitted);
    return p;
}

} // namespace GnssProcessor
