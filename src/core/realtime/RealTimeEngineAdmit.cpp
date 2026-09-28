/**
 * @file RealTimeEngineAdmit.cpp
 * @brief Admission gates for RealTimeEngine::processEpoch.
 *
 * The only seams that can end an epoch early.  Each returns false to mean
 * "processEpoch returns false".  Every early exit in these bodies was
 * already a `return false` in the pre-cut driver, so the relocation is
 * verbatim and the only control-flow change is the appended
 * `return true` � see scripts/rt_seam_verify.py, which proves that
 * mechanically against the frozen pre-cut source.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppUtils.hpp"
#include "products/SsrCorrectionStore.hpp"
#include <algorithm>
#include <chrono>

namespace GnssProcessor {

/**
 * @brief Real-time obs/wall-clock lag: backlog warn, obs<->SSR hold, skip-to-latest.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 835-915); see scripts/rt_seam_verify.py.
 */
bool RealTimeEngine::admitBacklogAndSync(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<RinexObservation>& epoch_obs = ctx.epoch_obs;

    const double now_gps = TimeUtils::nowContinuousGpsSeconds();
    const double obs_gps = TimeUtils::continuousGpsSeconds(current_time);
    const double lag_s   = now_gps - obs_gps;
    if (config_.rtBacklogWarnSeconds() > 0.0 && lag_s > config_.rtBacklogWarnSeconds()) {
        if ((backlog_warn_ctr_++ % 30) == 0) {
            // lag_s (wall clock against the obs epoch) is truncated to whole
            // seconds by nowContinuousGpsSeconds: adequate for the gates,
            // coarse for diagnosis.  The framer receipt stamp gives the
            // sub-second receive-to-process component (assembly wait plus
            // backlog queueing) on the steady clock, which wall-clock steps do
            // not affect.  Diagnostic only; no gate reads it (RTB-1(b)).
            double rx_to_proc_s = -1.0;
            if (processing_epoch_receipt_valid_) {
                rx_to_proc_s = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - processing_epoch_receipt_).count();
            }
            if (rx_to_proc_s >= 0.0)
                log->warn("{}obs backlog: epoch wk{} tow{:.1f} is {:.1f}s behind wall-clock GPS "
                          "(frame rx→process {:.3f}s; corrections will be fresher than these obs)",
                          epoch_prefix, current_time.week, current_time.tow, lag_s, rx_to_proc_s);
            else
                log->warn("{}obs backlog: epoch wk{} tow{:.1f} is {:.1f}s behind wall-clock GPS "
                          "(corrections will be fresher than these obs)",
                          epoch_prefix, current_time.week, current_time.tow, lag_s);
        }
    }

    // --- obs↔SSR sync: hold obs ahead of the SSR coverage horizon ---
    // PPP-RTK needs obs time-aligned with the corrections, not pinned to wall
    // clock.  An obs epoch newer than (freshest SSR content epoch + ssr_max_age)
    // has no SSR any satellite can use, so processing it now would make the whole
    // epoch broadcast-grade (and, with drop_broadcast_grade, drop all of it).  The
    // epoch is deferred instead; drainHeldObs() re-attempts it when the SSR
    // stream advances.  Liveness: if the feed stalls longer than
    // obs_ssr_sync_max_hold_s, a degraded "outage" state latches and every epoch
    // passes through (broadcast-grade, or dropped by the existing path) until SSR
    // covers the obs again, so a dead feed cannot starve the filter.  Applies to
    // live-SSR real-time only; never on the final flush drain or before the first
    // epoch.
    if (config_.enableObsSsrSync() && !first_epoch_ && !draining_final_
        && config_.realtimeUsesLiveOrbitClockSource()) {
        const auto newest = ssrStore().newestSsrEpoch();
        if (newest) {
            const double horizon = TimeUtils::continuousGpsSeconds(*newest)
                                 + config_.getSsrMaxAge();
            if (obs_gps <= horizon) {
                sync_hold_since_s_ = 0.0;   // SSR covers this epoch: resume normal
                sync_outage_       = false;
                held_obs_.clear();
            } else if (sync_outage_) {
                held_obs_.clear();          // confirmed outage: pass through degraded
            } else {
                if (sync_hold_since_s_ <= 0.0) sync_hold_since_s_ = now_gps;
                const double held_for = now_gps - sync_hold_since_s_;
                if (held_for <= config_.getObsSsrSyncMaxHoldS()) {
                    held_obs_        = epoch_obs;   // keep-latest (skip-to-latest semantics)
                    held_epoch_time_ = current_time;
                    if ((sync_hold_warn_ctr_++ % 30) == 0)
                        log->debug("{}obs↔SSR hold: obs tow{:.1f} ahead of SSR horizon "
                                   "(newest SSR content tow{:.1f} +{:.0f}s); held {:.1f}/{:.1f}s",
                                   epoch_prefix, current_time.tow, newest->tow,
                                   config_.getSsrMaxAge(), held_for,
                                   config_.getObsSsrSyncMaxHoldS());
                    return false;   // deferred; drained when SSR advances
                }
                // Held past the budget: the SSR feed is stalled.  Latch degraded.
                sync_outage_       = true;
                sync_hold_since_s_ = 0.0;
                held_obs_.clear();
                log->warn("{}obs↔SSR hold expired after {:.1f}s (SSR stream stalled); "
                          "processing degraded until SSR recovers", epoch_prefix, held_for);
            }
        }
    }

    if (config_.enableObsSkipToLatest() && !first_epoch_
        && !draining_final_ && !processing_synced_epoch_
        && lag_s > config_.obsSkipMaxLagS()) {
        log->debug("{}Skipping stale obs epoch TOW {:.3f} ({:.1f}s behind real-time)",
                   epoch_prefix, current_time.tow, lag_s);
        return false;
    }

    return true;
}

/**
 * @brief processing.max_duration_s data-time cutoff; latches max_duration_reached_.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 924-935); see scripts/rt_seam_verify.py.
 */
bool RealTimeEngine::gateMaxDuration(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;

    if (config_.getMaxDuration() > 0.0 && max_duration_anchored_) {
        const double span = TimeUtils::continuousGpsSeconds(current_time)
                          - max_duration_anchor_s_;
        if (span >= config_.getMaxDuration()) {
            if (!max_duration_reached_)
                log->info("{}Reached max processing duration: {:.1f} s of data processed "
                          "(limit {:.1f} s); ending session", epoch_prefix, span,
                          config_.getMaxDuration());
            max_duration_reached_ = true;
            return false;
        }
    }

    return true;
}

/**
 * @brief Time step / gap detection + the aiding-predict capture. False on a non-advancing TOW (duplicate epoch).
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 940-978); see scripts/rt_seam_verify.py.
 */
bool RealTimeEngine::stepTimeAndDetectGap(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    double& dt = ctx.dt;
    bool& gap_detected = ctx.gap_detected;

    if (first_epoch_) {
        dt = 1.0; // assume 1 s for first real-time epoch
        log->info("{}First epoch", epoch_prefix);
    } else {
        dt = current_time.tow - prev_tow_;
        if (dt < 0.0) dt += TimeUtils::SECONDS_IN_WEEK; // week rollover
        if (dt <= 0.0) {
            // Non-advancing TOW: the same physical epoch arriving a second time,
            // not a new time step.  One GNSS epoch reaches the engine as more than
            // one observation-bearing SBF block whenever the receiver co-logs
            // MeasEpoch (4027) and Meas3Ranges (4109): both decode to observations
            // at the identical TOW (SbfParser::parseFrame), and each drives
            // processEpoch through feedSbfFrame.  A retransmitted frame looks the
            // same.  Folding the duplicate in would (a) double-count its
            // measurements in the KF update and (b) feed dt=0 into
            // KalmanFilter::predict -> StateManager::getProcessNoiseMatrix, which
            // throws "Invalid time step: 0" and aborts the session.  The epoch is
            // skipped with prev_tow_ and the state untouched, so the next advancing
            // epoch steps from the last accepted one.
            log->debug("{}Skipping duplicate epoch at TOW {:.3f} (no time advance; "
                       "second co-timestamped SBF block or retransmit)",
                       epoch_prefix, current_time.tow);
            return false;
        }
        const double gap_tol = config_.getDataGapTolerance() > 0.0
                             ? config_.getDataGapTolerance()
                             : 1.0;  // default: 1 s (typical RT rate)
        gap_detected = dt > gap_tol;
    }
    // Capture the pre-update epoch boundary and first-epoch flag for aiding-source
    // prediction: prev_epoch_time_ and first_epoch_ are both overwritten before
    // the predict site, but predictAided needs the (prev, current] integration
    // interval and the first-epoch skip.  Unused on the GNSS-only path (empty
    // aiding manager).
    ctx.aiding_t_prev = prev_epoch_time_;
    ctx.aiding_first  = first_epoch_;

    prev_tow_ = current_time.tow;
    prev_epoch_time_ = current_time;

    return true;
}

/**
 * @brief Corrections-ready startup gate: defer all estimation until the cm-grade (broadcast+SSR) quorum is met (with require_all_above_mask, every modeled above-mask satellite) or the gate times out.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::processEpoch
 * (frozen lines 1019-1120); see scripts/rt_seam_verify.py.
 */
bool RealTimeEngine::gateCorrectionsReady(RtEpochContext& ctx) {
    auto log = log_manager_.getLogger("rt_engine");
    const std::string& epoch_prefix = ctx.epoch_prefix;
    const GnssTime& current_time = ctx.current_time;
    const std::vector<PppObservation>& modeled_obs = ctx.modeled_obs;

    if (config_.getCorrectionsReadyGate().enabled && !estimation_started_) {
        const auto& gate = config_.getCorrectionsReadyGate();

        // Per-constellation tally: cm-grade (broadcast+SSR, phase-retained) and
        // total above-mask counts per active constellation.
        std::map<std::string, int> cm_per_cons, total_per_cons;
        size_t n_cm = 0;
        std::vector<std::string> not_ready;
        for (const auto& o : modeled_obs) {
            const std::string cons = PppUtils::getSystemString(o.satellite);
            ++total_per_cons[cons];
            if (o.broadcast_only) { not_ready.push_back(o.satellite); }
            else { ++n_cm; ++cm_per_cons[cons]; }
        }

        // Per-constellation quorum: every configured constellation must field a
        // cm-grade SD quorum before estimation starts, including one with no
        // satellite present this epoch.  The loop therefore runs over
        // active_systems rather than total_per_cons: a constellation whose
        // broadcast ephemeris is still assembling contributes zero satellites
        // and is absent from total_per_cons.  Letting the gate latch on a
        // single constellation (Galileo only, while GPS had no orbit yet) lets
        // GPS join mid-run and perturb an already-converging filter, which drove
        // the ZWD<->height null-space runaway and flagged code.  Holding the gate
        // for the absent constellation forces a multi-system start, or the
        // timeout below forces a degraded one.  per_system <= 0 disables the
        // per-system check (flat global floor only).
        bool per_system_ok = true;
        std::vector<std::string> cons_waiting;
        if (gate.min_satellites_per_system > 0) {
            for (const auto& cons : config_.getActiveSystems()) {
                // GLONASS yields no SD rows when BSSD is active without the
                // GLONASS-BSSD opt-in, so it can never reach cm-grade quorum;
                // requiring it would stall every startup to the timeout.
                if (cons == "GLONASS" && config_.enableBetweenSatelliteSd()
                    && !config_.enableGlonassBssd()) continue;

                const int total = total_per_cons.count(cons) ? total_per_cons[cons] : 0;
                const int have  = cm_per_cons.count(cons)    ? cm_per_cons[cons]    : 0;
                // Smaller of the configured floor and what the constellation can
                // currently field, with the fielded count raised to 2 (an SD pair):
                // an absent or single-satellite constellation (total < 2) still
                // needs min(floor, 2) cm-grade satellites and holds the gate
                // until it shows up.
                const int need = std::min(gate.min_satellites_per_system,
                                          std::max(total, 2));
                if (have < need) {
                    per_system_ok = false;
                    cons_waiting.push_back(cons + " " + std::to_string(have) + "/" + std::to_string(need));
                }
            }
        }

        const bool ready = n_cm >= static_cast<size_t>(gate.min_satellites)
                        && per_system_ok
                        && (!gate.require_all_above_mask || not_ready.empty());

        if (++warmup_epochs_ == 1) warmup_start_time_ = current_time;
        bool timed_out = false;
        if (!ready && gate.timeout_s > 0.0) {
            const double elapsed = TimeUtils::continuousGpsSeconds(current_time)
                                 - TimeUtils::continuousGpsSeconds(warmup_start_time_);
            timed_out = elapsed >= gate.timeout_s;
        }

        std::string waiting;
        for (size_t i = 0; i < not_ready.size(); ++i) {
            if (i) waiting += ",";
            waiting += not_ready[i];
        }
        std::string cons_short;
        for (size_t i = 0; i < cons_waiting.size(); ++i) {
            if (i) cons_short += ", ";
            cons_short += cons_waiting[i];
        }

        if (!ready && !timed_out) {
            if (warmup_epochs_ == 1 || (warmup_epochs_ % 10) == 0) {
                log->info("{}Startup gate: deferring — {} cm-grade total (need >= {}), "
                          "per-constellation [{}], {} still broadcast-only [{}]",
                          epoch_prefix, n_cm, gate.min_satellites,
                          cons_short.empty() ? "ok" : cons_short,
                          not_ready.size(), waiting);
                // The SSR-store census localises the problem: zero orbit/clock
                // counts for a constellation indicate a decode/store gap; nonzero
                // counts while it stays broadcast-only indicate an IODE-match issue
                // (see the per-satellite "SSR clock present ... but NOT applied"
                // trace in GnssCorrections).
                log->info("{}  SSR store census: {}",
                          epoch_prefix, ssrStore().censusSummary());
            }
            return false;   // accumulate nav/SSR; no predict/update; first_epoch_ stays true
        }

        estimation_started_ = true;
        if (timed_out)
            log->warn("{}Startup gate: timeout ({:.0f}s) — {} sat(s) still broadcast-only; "
                      "starting in DEGRADED mode", epoch_prefix, gate.timeout_s, not_ready.size());
        else
            log->info("{}Startup gate satisfied: {} cm-grade sat(s) across {} constellation(s) — "
                      "starting estimation (code+phase together){}",
                      epoch_prefix, n_cm, cm_per_cons.size(),
                      not_ready.empty() ? "" : " (broadcast-only stragglers join as SSR arrives)");
    }

    return true;
}

} // namespace GnssProcessor
