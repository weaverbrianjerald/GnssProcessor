/**
 * @file RealTimeEngine.cpp
 * @brief Real-time GNSS PPP processing engine implementation.
 *
 * Mirrors the per-epoch logic of Application (post-processing), structured
 * for epoch-at-a-time streaming operation rather than batch iteration.
 */

#include "realtime/RealTimeEngine.hpp"
#include "realtime/RtStateSnapshot.hpp"  // complete type for staged_rt_snap_'s destructor
#include "aiding/ImuAidingSource.hpp"   // full type for aiding_manager_->imu() wiring
#include "app/EpochUpdateSupport.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "sbf/SbfParser.hpp"
#include "math/Coordinates.hpp"
#include "measurement/StateInitializer.hpp"   // systemToIsbName (clock-disc ISB scoping)
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <fmt/format.h>

namespace GnssProcessor {

namespace {

/// Config-section name for a constellation, or nullptr if not user-selectable.
const char* systemConfigName(System sys) {
    switch (sys) {
    case System::GPS:     return "GPS";
    case System::Galileo: return "Galileo";
    case System::GLONASS: return "GLONASS";
    case System::BeiDou:  return "BeiDou";
    case System::QZSS:    return "QZSS";
    case System::SBAS:    return "SBAS";
    default:              return nullptr;
    }
}

/// True iff @p sys appears in the configured active-systems list.
bool isSystemActive(System sys, const std::vector<std::string>& active) {
    const char* name = systemConfigName(sys);
    if (!name) return false;
    return std::find(active.begin(), active.end(), name) != active.end();
}

/// Push the live store's GLONASS FDMA channel map (R## → k) into PppModel
/// whenever this frame delivered GLONASS broadcast ephemeris.  Shared by the
/// RTCM and SBF ingest paths so both seed per-satellite GLONASS wavelengths
/// the same way: from the live 1020 / GLONav @c freq_channel, not the
/// hard-coded default table.  No-op when the frame carried no GLONASS nav.
void refreshGlonassSlots(const std::vector<BroadcastEphemeris>& fresh,
                         LiveEphemerisStore& store, PppModel& model) {
    for (const auto& e : fresh) {
        if (e.system == System::GLONASS) {
            model.setGlonassSlots(store.glonassFreqChannels());
            return;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RealTimeEngine::RealTimeEngine(GnssConfig& config, LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
}

// Out-of-line so the staged_rt_snap_ unique_ptr's deleter sees the complete
// RtStateSnapshot type (forward-declared in the header).
RealTimeEngine::~RealTimeEngine() = default;

// ---------------------------------------------------------------------------
// GPS -> UTC helper
// ---------------------------------------------------------------------------

GnssTime RealTimeEngine::gpsToUtcTime(const GnssTime& gps_time) {
    const double gps_secs = TimeUtils::continuousGpsSeconds(gps_time);
    const double utc_secs = gps_secs - TimeUtils::LEAP_SECONDS;
    int utc_week = static_cast<int>(std::floor(utc_secs / TimeUtils::SECONDS_IN_WEEK));
    double utc_tow = utc_secs - static_cast<double>(utc_week) * TimeUtils::SECONDS_IN_WEEK;
    if (utc_tow < 0.0) { utc_tow += TimeUtils::SECONDS_IN_WEEK; --utc_week; }
    return GnssTime{ utc_week, utc_tow };
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool RealTimeEngine::initialize() {
    auto log = log_manager_.getLogger("rt_engine");
    log->info("Initializing real-time engine");

    // Construction phase.  The call order is a dependency chain; the seam
    // block in RealTimeEngine.hpp lists what depends on what.  In particular,
    // seedInitialState() must follow initProcessingComponents().
    armSsrStoreOptions();
    if (!initProducts()) return false;
    initStateManagerAndApriori();
    initProcessingComponents();
    initAmbiguityResolver();
    seedInitialState();
    initTraceLoggers();
    // RT filter-state snapshot (consumer warm start): parse and stage only.
    // The fingerprint depends on post-initialize setters (setSsrStore etc.)
    // and the age gate needs a real epoch, so the snapshot is applied at the
    // first processed epoch.  No-op with realtime.state_snapshot_path empty.
    loadStateSnapshotStaged_();

    log->info("Real-time engine initialized (position seeded at ({:.1f}, {:.1f}, {:.1f}))",
              initial_position_.x(), initial_position_.y(), initial_position_.z());
    return true;
}

// ---------------------------------------------------------------------------
// RTCM frame ingestion
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SSR ingest filter: a pure record-kind → push-action policy matrix, kept
// outside feedRtcmFrame so it is unit-testable (record in, push decision out)
// without a parser or an engine (RTA-8(b)).
// ---------------------------------------------------------------------------

RealTimeEngine::SsrPushAction
RealTimeEngine::ssrPushAction(SsrAcceptPolicy policy, SsrRecordKind kind) {
    if (policy == SsrAcceptPolicy::All) return SsrPushAction::Push;
    // OrbitOnly: orbit and URA records are pushed; combined records are
    // decomposed at the push site (they never enter the store, so its
    // combined-wins merge is untouched); every record carrying clock or bias
    // content is dropped, including the high-rate clock, which is an additive
    // delta on the clock polynomial.
    switch (kind) {
        case SsrRecordKind::Orbit:
        case SsrRecordKind::Ura:            return SsrPushAction::Push;
        case SsrRecordKind::Combined:       return SsrPushAction::DecomposeOrbit;
        case SsrRecordKind::Clock:
        case SsrRecordKind::CodeBias:
        case SsrRecordKind::PhaseBias:
        case SsrRecordKind::HighRateClock:  return SsrPushAction::Drop;
    }
    return SsrPushAction::Drop;   // unreachable; keeps -Wreturn-type quiet
}

SsrOrbitCorrection
RealTimeEngine::combinedOrbitPart(const SsrCombinedCorrection& rec) {
    SsrOrbitCorrection orb;
    orb.sat             = rec.sat;
    orb.iode            = rec.iode;
    orb.delta_rac       = rec.delta_rac;
    orb.delta_rac_dot   = rec.delta_rac_dot;
    orb.received_time   = rec.received_time;
    orb.epoch_time      = rec.epoch_time;    // shared orbit+clock content epoch → orbit dt base
    orb.iod_ssr         = rec.iod_ssr;
    orb.reference_point = rec.reference_point;  // the orbit part keeps its dialect tag (RTB-5(b))
    // c0/c1/c2 are discarded: the decomposition exists so that no clock content
    // from this stream reaches the store.
    return orb;
}

RealTimeEngine::SsrAcceptPolicy
RealTimeEngine::parseSsrAcceptPolicy(const std::string& value, bool* recognized) {
    std::string v;
    v.reserve(value.size());
    for (char c : value) v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (recognized) *recognized = true;
    if (v.empty() || v == "all")  return SsrAcceptPolicy::All;
    if (v == "orbit_only")        return SsrAcceptPolicy::OrbitOnly;
    if (recognized) *recognized = false;
    return SsrAcceptPolicy::All;
}

void RealTimeEngine::feedRtcmFrame(const std::vector<uint8_t>& frame,
                                   SsrAcceptPolicy accept,
                                   std::chrono::steady_clock::time_point receipt) {
    // Decode the frame through the engine's persistent parser (RTB-1(a)).
    // Outputs are drained per frame (clear() below), but the parser's
    // stream-time tracker survives, so mod-week epoch fields are promoted by
    // nearest continuation to the last decoded epoch instead of re-anchoring
    // on the host clock every frame.  All three RTCM streams (primary,
    // corrections, ephemeris) share this parser, and so one GPS timeline.
    rtcm_parser_.clear();
    RtcmParser& parser = rtcm_parser_;
    parser.parseFrame(frame);

    // MSM lock-time → LLI 0x1 derivation, armed by
    // realtime.msm_lli_from_locktime (default off; arming it changes
    // slip-detection behaviour on RT replays).  (RTB-3)
    const bool lli_from_locktime =
        config_.getRealTimeConfig().msm_lli_from_locktime;

    // Observations: buffer until a new epoch is detected, then submit the
    // previous epoch.
    // Disabled constellations are dropped before the epoch-boundary test, not
    // only in processEpoch.  A disabled constellation whose epoch is tagged on
    // a different time base (for example a GLONASS MSM epoch that could not be
    // promoted to a GPS week and comes back as {0, 0}) carries a very different
    // tow and would split a coherent epoch in two: the leading systems would be
    // submitted early without the trailing ones, and the trailing systems would
    // then arrive at the tow of the just-submitted epoch and be dropped by
    // processEpoch's dt == 0 duplicate guard (for example, Galileo lost from a
    // GPS+Galileo run with GLONASS present but disabled).
    const auto& active = config_.getActiveSystems();
    for (const auto& obs : parser.getObservations()) {
        if (!isSystemActive(obs.system, active)) continue;
        if (pending_obs_.empty()) {
            pending_epoch_time_ = obs.time;
            // The assembling epoch's receipt is the stamp of the first
            // frame that contributed observations to it (RTB-1(b)).
            pending_epoch_receipt_       = receipt;
            pending_epoch_receipt_valid_ = true;
        } else if (std::abs(obs.time.tow - pending_epoch_time_.tow) > 0.5) {
            // Carry the buffered epoch's receipt stamp into processEpoch for
            // the sub-second latency diagnostics (RTB-1(b)).
            processing_epoch_receipt_       = pending_epoch_receipt_;
            processing_epoch_receipt_valid_ = pending_epoch_receipt_valid_;
            // New epoch: process the buffered one.
            processEpoch(pending_obs_);
            processing_epoch_receipt_valid_ = false;
            pending_obs_.clear();
            pending_epoch_time_          = obs.time;
            pending_epoch_receipt_       = receipt;   // new epoch's first frame (RTB-1(b))
            pending_epoch_receipt_valid_ = true;
        }
        pending_obs_.push_back(obs);
        if (lli_from_locktime) {
            // Derive the loss-of-lock bit from the decoded MSM lock times on
            // the stored copy, never the parser's (RTB-3).  The station key is
            // "" because this engine serves a single rover; the multi-station
            // front-end reuses one LockTimeTracker across streams with
            // per-stream station keys.
            auto& stored = pending_obs_.back();
            for (const auto& [code, lock_ms] : stored.lock_time_ms) {
                if (lock_time_tracker_.update("", stored.satellite, code, lock_ms)) {
                    stored.lli_flags[code] |= 0x1;
                }
            }
        }
    }

    const std::size_t n_ssr_dropped = ingestSsrRecords(parser, accept);

    // Push decoded broadcast ephemeris into the live store, from which
    // Geometry::getSatellitePV resolves satellite orbits when no SP3 file is loaded.
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph);
    refreshGlonassSlots(parser.getBroadcastEphemeris(), eph_store, *ppp_model_);

    logRtcmIngestCounters(parser, n_ssr_dropped);

    seedAprioriFromStationCoords(parser);
}

// ---------------------------------------------------------------------------
// Direct observation feed
// ---------------------------------------------------------------------------

void RealTimeEngine::feedSbfFrame(const std::vector<uint8_t>& block) {
    SbfParser parser;
    parser.parseFrame(block);

    // Seed the filter a-priori from the receiver's own PVTGeodetic solution
    // until the KF has bootstrapped.  A live SBF stream carries no RINEX
    // APPROX POSITION, so without this the a-priori is the coarse Earth-centred
    // fallback (geographic pole) from which almost every satellite sits below
    // the elevation mask and the filter can never converge.  A configured
    // reference_position always wins; once first_epoch_ clears, the KF owns the
    // position and this seeding stops.
    if (first_epoch_ && !config_.hasReferencePosition()) {
        if (const auto& pvt = parser.getReceiverPositionEcef()) {
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
                sv.segment<3>(pidx) = *pvt;
                state_manager_->setStateVector(sv);
                ppp_state_.position = *pvt;
                if (!apriori_seeded_from_pvt_) {
                    log_manager_.getLogger("rt_engine")->info(
                        "Seeded a-priori from PVTGeodetic: ({:.3f}, {:.3f}, {:.3f}) m",
                        pvt->x(), pvt->y(), pvt->z());
                    apriori_seeded_from_pvt_ = true;
                }
            }
        }
    }

    // Broadcast ephemeris -> live store (the same path RTCM nav uses).  These
    // are the receiver's decoded nav blocks, emitted only on ephemeris change.
    // from_igs=false: receiver nav must not overwrite an authoritative
    // IGS-stream record, because the IGS-RTS SSR is referenced to the IGS
    // broadcast and pairing it with a receiver ephemeris is inconsistent (see
    // LiveEphemerisStore::insert).
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const auto& eph : parser.getBroadcastEphemeris()) eph_store.insert(eph, /*from_igs=*/false);
    refreshGlonassSlots(parser.getBroadcastEphemeris(), eph_store, *ppp_model_);
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "SBF block {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastBlockNumber(), parser.getBroadcastEphemeris().size(), eph_store.size());
    }

    // Raw navigation subframes -> assemble ephemeris -> live store.  Unlike the
    // on-change decoded-nav blocks above, the receiver streams these
    // continuously, so this is the primary ephemeris source for a live SBF run.
    for (const auto& m : parser.getRawNavMessages()) {
        if (auto eph = subframe_assembler_.consume(m)) {
            eph_store.insert(*eph, /*from_igs=*/false);   // receiver nav: never clobbers IGS-stream eph
            log_manager_.getLogger("rt_engine")->trace(
                "SBF raw-nav: assembled {} ephemeris (iode {}, {} sats in live store)",
                eph->satellite, eph->iode, eph_store.size());
        }
    }

    // A MeasEpoch carries a complete epoch of observations, so it is processed
    // directly (no multi-message assembly as in the RTCM/MSM path).
    const auto& obs = parser.getObservations();
    if (!obs.empty()) {
        processEpoch(obs);
    }
}

void RealTimeEngine::feedObservations(const std::vector<RinexObservation>& observations) {
    processEpoch(observations);
}

void RealTimeEngine::feedImuSample(const AidingSample& sample) {
    // Buffer into the IMU aiding source (no-op when no IMU source is active);
    // consumed by the next epoch's predictAided.
    if (aiding_manager_) aiding_manager_->ingestSample(sample);
}

void RealTimeEngine::flush() {
    // Final drain at stream stop: process whatever is buffered without the
    // obs↔SSR hold or skip-to-latest (no later epoch exists to defer to).  A held
    // sync epoch (older) is submitted before the still-assembling pending epoch
    // (newer) so they reach the filter in time order.
    draining_final_ = true;
    if (!held_obs_.empty()) {
        processEpoch(held_obs_);
        held_obs_.clear();
    }
    if (!pending_obs_.empty()) {
        // The final buffered epoch still carries its receipt stamp (RTB-1(b)).
        processing_epoch_receipt_       = pending_epoch_receipt_;
        processing_epoch_receipt_valid_ = pending_epoch_receipt_valid_;
        processEpoch(pending_obs_);
        processing_epoch_receipt_valid_ = false;
        pending_obs_.clear();
        pending_epoch_receipt_valid_ = false;
    }
    draining_final_ = false;

    // End-of-run obs-code attribute-mismatch summary (flush is the engine's
    // end-of-stream hook).  Silent when no mismatch was counted.  (RTA-13)
    logBiasAttrMismatchSummary("end of run");
    // The same summary for the orbit/clock coherence-echo mismatches (card 7).
    logOrbitEchoMismatchSummary("end of run");
    // The declared-step carry counters, silent unless armed (card CC (A)).  The
    // closed-loop harness never calls flush(); its consumer's counters go out on
    // the closed-loop summary in NetworkTeardown instead.
    logPbDiscCarrySummary_("end of run");
    // The every-step translation counters (card PBT (A)).
    logPbStepTranslateSummary_("end of run");
    // The hold reaction, silent unless a held entry was seen (card PBH).
    logPbHoldSummary_("end of run");
    // The ambiguity level-check counters (card ALC).
    logAmbLevelCheckSummary_("end of run");
    // The declared-roster census, silent unless a declaration was seen or the
    // 5b knob is armed (card SID phase 5).  The closed-loop harness and the
    // replay driver never call flush(), so both call this directly at their
    // own tails.
    logRosterConsumerSummary("end of run");
    logReinflateSummary("end of run");   // card OWD (O2)
}

void RealTimeEngine::drainHeldObs() {
    if (held_obs_.empty()) return;
    // Move out before re-submitting: processEpoch may re-hold (re-assign held_obs_)
    // if the SSR stream has not yet advanced far enough to cover this epoch.
    std::vector<RinexObservation> obs = std::move(held_obs_);
    held_obs_.clear();
    processing_synced_epoch_ = true;   // bypass skip-to-latest for the SSR-aligned epoch
    processEpoch(obs);
    processing_synced_epoch_ = false;
}

// ---------------------------------------------------------------------------
// Process a single epoch
// ---------------------------------------------------------------------------

bool RealTimeEngine::processEpoch(const std::vector<RinexObservation>& epoch_obs) {
    auto log = log_manager_.getLogger("rt_engine");

    if (epoch_obs.empty()) return false;

    RtEpochContext ctx{ epoch_obs };
    ctx.current_time = epoch_obs.front().time;
    state_manager_->setStateEpochTime(ctx.current_time);
    LogManager::setCurrentEpochTime(ctx.current_time);

    // Clear the previous epoch's per-satellite readout here, before any gate can
    // end the epoch, so an epoch the admission gates defer leaves the buffer
    // empty (that epoch produced no consumer solution) instead of handing the
    // harness a stale repeat.  No-op unarmed.  (card CST)
    if (consumer_sat_trace_) {
        consumer_sat_rows_.clear();
        consumer_sat_events_.clear();
    }

    // The same contract for the level check's per-epoch transport: a deferred
    // epoch hands the harness an empty event list and a zero check count, never
    // the previous epoch's.  No-op unarmed.  (card ALC)
    if (config_.ambLevelCheck()) {
        amb_level_events_.clear();
        amb_level_epoch_checks_ = 0;
    }

    // Mode tag only; the spdlog %E field already carries the GPS time.
    // "[RT] " distinguishes realtime-engine output from post-processing
    // when both modes share a console.
    ctx.epoch_prefix = "[RT] ";

    // RT filter-state snapshot: one-shot staged warm resume, applied before the
    // admission gates.  Restoring the previous-epoch time here lets
    // stepTimeAndDetectGap compute the first dt over the true outage gap, and
    // an epoch the gates then defer leaves the applied state waiting like any
    // other quiet epoch.  No-op unless a snapshot was staged.
    applyStagedStateSnapshot_(ctx);

    // --- Admission gates ---------------------------------------------------
    // The only steps that can end the epoch early, in this required order:
    // lag/sync deferral, then the data-time budget, then the time step (whose
    // duplicate-TOW guard must precede any state advance), then the
    // corrections-ready startup gate, which needs modeled observations and so
    // runs after computeModels below.
    if (!admitBacklogAndSync(ctx)) return false;
    if (!gateMaxDuration(ctx))     return false;
    if (!stepTimeAndDetectGap(ctx)) return false;

    // --- Sync PPP state ---
    try {
        state_manager_->updatePppStateFromVector(
            state_manager_->getStateVector(), ppp_state_, *state_manager_);
    } catch (const std::exception& e) {
        log->error("{}State sync failed: {}", ctx.epoch_prefix, e.what());
        return false;
    }

    // --- Compute modeled observations ---
    // Filter to active systems.  Kept in the driver: it is the epoch's input
    // conditioning, and isSystemActive is a file-local helper of this TU.
    std::vector<RinexObservation> filtered_obs;
    {
        const auto& active = config_.getActiveSystems();
        for (const auto& obs : epoch_obs) {
            if (isSystemActive(obs.system, active)) filtered_obs.push_back(obs);
        }
    }
    if (filtered_obs.empty()) {
        log->debug("{}No observations after system filtering", ctx.epoch_prefix);
        return false;
    }

    // The producer's declared signal roster reaches the latch here and nowhere
    // else: computeModels decides a constellation's canonical signals on its
    // first epoch, so a declaration handed over after this call is too late for
    // the band it would have moved.  No-op on a wire that carries no roster
    // (producers without the declaration, every RTCM stream).  (card SID phase 5)
    refreshDeclaredRoster_();

    if (!ppp_model_->computeModels(filtered_obs, ppp_state_, ctx.modeled_obs,
                                   &ctx.drop_reasons)) {
        log->warn("{}Failed to compute models", ctx.epoch_prefix);
        return false;
    }

    if (!gateCorrectionsReady(ctx)) return false;

    // --- Observability -----------------------------------------------------
    // Both run before the broadcast-grade drop so dropped sats still appear.
    logSsrCoverage(ctx);
    traceSsrConsumer(ctx);

    // --- Measurement-set conditioning --------------------------------------
    // seedClockFromWls and regaugeBssd are mutually exclusive (hasClock() vs
    // not); each self-gates, so both are called unconditionally here.
    dropBroadcastGrade(ctx);
    seedClockFromWls(ctx);
    regaugeBssd(ctx);

    // First real epoch processed: clear first_epoch_ unconditionally, not inside
    // the hasClock() WLS seeding, because BSSD / PPP-RTK has no receiver-clock
    // state and skips that step.  A flag left set keeps dt pinned at 1 s (gaps
    // go undetected) and re-seeds the PVTGeodetic a-priori every epoch, which
    // pins the position so the filter cannot converge.  Cold-start epochs with
    // no modelable observations return earlier, so reaching here means this
    // epoch produced a usable measurement set.
    first_epoch_ = false;

    // Anchor the max-duration window on this first fully-processed epoch (epoch
    // GPS time relative to first obs).  Deferred warmup epochs return before
    // here, so the budget starts at real estimation rather than cold start.
    if (config_.getMaxDuration() > 0.0 && !max_duration_anchored_) {
        max_duration_anchor_s_ = TimeUtils::continuousGpsSeconds(ctx.current_time);
        max_duration_anchored_ = true;
    }

    // --- KF predict ---
    // Aiding-augmented prediction, shared with post-processing.  GNSS-only
    // stream => empty manager => predictAided delegates to predict(dt),
    // byte-identical.  IMU samples (fed via feedImuSample) are integrated over
    // (aiding_t_prev, current_time].
    if (aiding_manager_ && !aiding_manager_->empty() && !ctx.aiding_first) {
        const Eigen::VectorXd aiding_x = state_manager_->getStateVector();
        const AidingEpochContext aiding_ctx{
            aiding_x, ctx.aiding_t_prev, ctx.current_time, ctx.aiding_first };
        kalman_filter_->predictAided(ctx.dt, aiding_manager_->collectPredict(aiding_ctx));
    } else {
        kalman_filter_->predict(ctx.dt);
    }
    state_manager_->updatePppStateFromVector(
        state_manager_->getStateVector(), ppp_state_, *state_manager_);

    detectAndRepairSlips(ctx);

    // --- SSR datum-break reactions -----------------------------------------
    // Reactions to the producer moving its datum.  The order is load-bearing:
    // the global generation-id break subsumes the fine-grained reactions that
    // follow, so it must run first.  All five no-op without SSR content.
    reactToGenerationChange(ctx);
    logProducerStatus(ctx);
    resetOnPhaseBiasDisc(ctx);
    reinflateOnCodeBiasDisc(ctx);
    reinflateOnClockDisc(ctx);

    // --- Estimation tail ---------------------------------------------------
    runMeasurementUpdate(ctx);

    // Soft-LLI bridge: roll the "last good" ambiguity snapshot from the
    // converged post-update state (the pre-gap anchor a later LLI hold repairs
    // against).  Only converged keys update it.  No-op unless turbo_edit_bridge_lli.
    if (soft_lli_)
        soft_lli_->recordSnapshot(*state_manager_,
                                  TimeUtils::continuousGpsSeconds(ctx.current_time));

    // Both pseudo-observation constraints sit after the measurement update and
    // before AR, so the shrunk covariance feeds the resolver.
    applyZwdConstraint(ctx);
    applyNetworkIonoPrior(ctx);

    resolveAmbiguities(ctx);
    writeEpochTraces(ctx);

    // Re-validate this epoch's float ambiguities against their own windowed code
    // level and refloat the ones the code rejects: invariant #7's re-coupling
    // edge for the card-CC carry and the card-PBT translation (card ALC).  It
    // sits after the update, constraints and AR (it reads the epoch's committed
    // pre-fit innovations) and before the CST harvest (so the row reports the
    // refloat_level event and the re-inflated amb_var).  The epoch's own
    // position is untouched: a refloat re-inflates the variance and marks the
    // key pending re-seed, which lands at the next epoch's ensureRequiredStates.
    // No-op unless processing.amb_level_check.
    checkAmbiguityLevel_(ctx);

    // Harvest the per-satellite readout last (after the update, the
    // pseudo-observation constraints and AR), so the ambiguity / slant-iono
    // values and variances it reports are the epoch's committed ones (card CST).
    // Measure-only; no-op unless network.trace_consumer_sat armed the engine.
    traceConsumerSat_(ctx);

    // The live filter memory is now "as of" this epoch: the stamp that
    // writeStateSnapshotTo uses and checks against the network copy (card CC (B)).
    last_processed_epoch_ = ctx.current_time;
    has_processed_epoch_  = true;

    // RT filter-state snapshot: cadence-gated atomic write of the live filter
    // memory, after the epoch's full estimation tail so the snapshot carries
    // this epoch's posterior.  No-op with realtime.state_snapshot_path empty.
    writeStateSnapshot_(ctx);

    return true;
}


// ---------------------------------------------------------------------------
// State accessors
// ---------------------------------------------------------------------------

const SsrCorrectionStore& RealTimeEngine::ssrStore() const {
    return ssr_store_override_ ? *ssr_store_override_
                              : SsrCorrectionStore::getInstance();
}

void RealTimeEngine::setSsrStore(const SsrCorrectionStore* store,
                                 bool consume_clock, bool consume_bias,
                                 SsrOrbitSource orbit_source) {
    // The clock/orbit overlay lives in PppModel (geometry + corrections); the
    // bias overlay and the phase-bias-discontinuity reset hook live in
    // MeasurementModel.  Each is routed independently so the caller can, e.g.,
    // keep precise SP3/CLK for the clock while consuming only the network phase
    // bias (clean integer test).  Called after initialize(), so the models exist.
    ssr_store_override_ = consume_bias ? store : nullptr;   // discontinuity hook reads phase bias
    // Remember the clock routing (card 7): the orbit-echo mismatch tallies land
    // on the store GnssCorrections reads clocks from, so the echo summary must
    // read that store (it differs from the bias-routed override above whenever
    // consume_clock != consume_bias).
    clock_store_override_ = consume_clock ? store : nullptr;
    if (ppp_model_) {
        switch (orbit_source) {
        case SsrOrbitSource::FollowClock:
            // Coupled routing (the default): the orbit overlay follows the
            // clock store.  The closed-loop harness call site takes this path,
            // byte-identical to the routing before the orbit source was split.
            ppp_model_->setSsrStore(consume_clock ? store : nullptr);
            break;
        case SsrOrbitSource::External:
            // Clocks from the injected store (per consume_clock); the orbit
            // overlay is pinned to the singleton store, which holds the external
            // RTS corrections stream (ideally ingest-filtered to orbit_only,
            // RTA-8(b)).  (RTA-8(a))
            ppp_model_->setSsrClockStore(consume_clock ? store : nullptr);
            ppp_model_->setSsrOrbitStore(nullptr);
            break;
        case SsrOrbitSource::Network:
            // For an orbit-bearing network product (not yet produced): the orbit
            // overlay reads the injected store even when the clock is kept
            // precise.  (RTA-8(a))
            ppp_model_->setSsrClockStore(consume_clock ? store : nullptr);
            ppp_model_->setSsrOrbitStore(store);
            break;
        }
    }
    if (meas_model_) meas_model_->setSsrStore(consume_bias  ? store : nullptr);
}

void RealTimeEngine::logBiasAttrMismatchSummary(const char* tag) {
    const auto counts = ssrStore().biasAttributeMismatchCounts();
    if (counts.empty()) return;   // clean run: never logs (byte-identical)
    std::uint64_t code_total = 0, phase_total = 0;
    for (const auto& [key, n] : counts) {
        if (!key.empty() && key.back() == 'L') phase_total += n;
        else                                   code_total  += n;
    }
    // Top offenders by count (deterministic tie-break: map key order).
    std::vector<std::pair<std::string, std::uint64_t>> top(counts.begin(), counts.end());
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr std::size_t kMaxListed = 8;
    std::string detail;
    for (std::size_t i = 0; i < top.size() && i < kMaxListed; ++i) {
        if (!detail.empty()) detail += ", ";
        detail += fmt::format("{}={}", top[i].first, top[i].second);
    }
    log_manager_.getLogger("rt_engine")->info(
        "[RT] SSR bias attribute mismatches ({}): tracked signal missing while a same-band "
        "sibling bias exists — code {} / phase {} lookups; top: {}{}",
        tag, code_total, phase_total, detail,
        top.size() > kMaxListed ? ", ..." : "");
}

void RealTimeEngine::logOrbitEchoMismatchSummary(const char* tag) {
    // Read the clock-routed store: GnssCorrections tallies the echo mismatches
    // there (the engine-level ssrStore() is bias-routed and stays empty under a
    // clock-on/bias-off consumer profile).
    const SsrCorrectionStore& clock_store =
        clock_store_override_ ? *clock_store_override_
                              : SsrCorrectionStore::getInstance();
    const auto counts = clock_store.orbitEchoMismatchCounts();
    if (counts.empty()) return;   // no mismatches counted: never logs (byte-identical)
    std::uint64_t total = 0;
    for (const auto& [key, n] : counts) total += n;
    // Top offenders by count (deterministic tie-break: map key order).
    std::vector<std::pair<std::string, std::uint64_t>> top(counts.begin(), counts.end());
    std::stable_sort(top.begin(), top.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr std::size_t kMaxListed = 8;
    std::string detail;
    for (std::size_t i = 0; i < top.size() && i < kMaxListed; ++i) {
        if (!detail.empty()) detail += ", ";
        detail += fmt::format("{}={}", top[i].first, top[i].second);
    }
    const double budget = clock_store.orbitEchoMaxSkew();
    log_manager_.getLogger("rt_engine")->info(
        "[RT] orbit/clock coherence-echo mismatches ({}): {} clock records whose orbit "
        "echo disagreed with the applied SSR orbit ({}); top: {}{}",
        tag, total,
        budget > 0.0
            ? fmt::format("withheld — budget {:.1f}s", budget)
            : "count-only — orbit_clock_max_skew_s unset, clocks still applied",
        detail, top.size() > kMaxListed ? ", ..." : "");
}

Eigen::Vector3d RealTimeEngine::getPosition() const {
    return ppp_state_.position;
}

double RealTimeEngine::getClockBias() const {
    return ppp_state_.clock_bias;
}

double RealTimeEngine::getTroposphereZwd() const {
    return ppp_state_.troposphere_zwd;
}

std::map<std::string, std::pair<double, double>> RealTimeEngine::getSystemTimeOffsets() const {
    std::map<std::string, std::pair<double, double>> out;
    if (!state_manager_) return out;
    const auto& isb = state_manager_->getSystemTimeOffsetStates();
    if (isb.empty()) return out;
    const Eigen::VectorXd x = state_manager_->getStateVector();
    for (const auto& [sys, idx] : isb) {
        if (idx >= static_cast<size_t>(x.size())) continue;
        const double var = state_manager_->getStateVariance(static_cast<int>(idx));
        out[sys] = { x(static_cast<Eigen::Index>(idx)), var > 0.0 ? std::sqrt(var) : 0.0 };
    }
    return out;
}

} // namespace GnssProcessor
