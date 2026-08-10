/**
 * @file RealTimeEngineIngest.cpp
 * @brief RTCM ingest seams for RealTimeEngine::feedRtcmFrame().
 *
 * The SSR push, its counters and the reference-station a-priori seed.
 *
 * The observation-buffering loop and the broadcast-ephemeris push stay in
 * the driver TU on purpose: both call anonymous-namespace helpers
 * (isSystemActive, refreshGlonassSlots) that are deliberately file-local
 * to RealTimeEngine.cpp.
 */

#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/LiveEphemerisStore.hpp"
#include <cmath>

namespace GnssProcessor {

/**
 * @brief Push decoded SSR records into the shared store under the per-stream RTA-8(b) accept policy.  Returns the count dropped by that policy.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::feedRtcmFrame
 * (frozen lines 518-542); see scripts/rt_seam_verify.py.
 */
std::size_t RealTimeEngine::ingestSsrRecords(RtcmParser& parser, SsrAcceptPolicy accept) {
    auto& ssr_store = SsrCorrectionStore::getInstance();
    const SsrHeader& hdr = parser.getLastSsrHeader();

    std::size_t n_ssr_dropped = 0;
    auto allowed = [&](SsrRecordKind kind) {
        if (ssrPushAction(accept, kind) == SsrPushAction::Push) return true;
        ++n_ssr_dropped;
        return false;
    };
    for (const auto& r : parser.getSsrOrbits())
        if (allowed(SsrRecordKind::Orbit))         ssr_store.pushOrbit(r, hdr);
    for (const auto& r : parser.getSsrClocks())
        if (allowed(SsrRecordKind::Clock))         ssr_store.pushClock(r, hdr);
    for (const auto& r : parser.getSsrCombined()) {
        switch (ssrPushAction(accept, SsrRecordKind::Combined)) {
            case SsrPushAction::Push:           ssr_store.pushCombined(r, hdr); break;
            case SsrPushAction::DecomposeOrbit: ssr_store.pushOrbit(combinedOrbitPart(r), hdr); break;
            case SsrPushAction::Drop:           ++n_ssr_dropped; break;
        }
    }
    for (const auto& r : parser.getSsrCodeBiases())
        if (allowed(SsrRecordKind::CodeBias))      ssr_store.pushCodeBias(r, hdr);
    for (const auto& r : parser.getSsrPhaseBiases())
        if (allowed(SsrRecordKind::PhaseBias))     ssr_store.pushPhaseBias(r, hdr);
    for (const auto& r : parser.getSsrUras())
        if (allowed(SsrRecordKind::Ura))           ssr_store.pushUra(r, hdr);
    for (const auto& r : parser.getSsrHighRateClocks())
        if (allowed(SsrRecordKind::HighRateClock)) ssr_store.pushHighRateClock(r, hdr);

    return n_ssr_dropped;
}

/**
 * @brief Per-frame SSR / ephemeris ingest counters, and the obs<->SSR drain trigger that fires when the SSR horizon advances.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::feedRtcmFrame
 * (frozen lines 550-582); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::logRtcmIngestCounters(RtcmParser& parser, std::size_t n_ssr_dropped) {
    auto& ssr_store = SsrCorrectionStore::getInstance();
    const SsrHeader& hdr = parser.getLastSsrHeader();
    auto& eph_store = LiveEphemerisStore::getInstance();

    const std::size_t n_ssr =
        parser.getSsrOrbits().size()      + parser.getSsrClocks().size() +
        parser.getSsrCombined().size()    + parser.getSsrCodeBiases().size() +
        parser.getSsrPhaseBiases().size() + parser.getSsrUras().size() +
        parser.getSsrHighRateClocks().size();
    // RTA-8(b): only records that actually reached the store count as "pushed"
    // (a decomposed combined record did — as an orbit).  With the default All
    // policy nothing is dropped and this is exactly the pre-filter n_ssr.
    const std::size_t n_ssr_pushed = n_ssr - n_ssr_dropped;
    if (n_ssr_pushed > 0) {
        if (n_ssr_dropped > 0)
            log_manager_.getLogger("rt_engine")->trace(
                "RTCM {}: pushed {} SSR records ({} dropped by ingest policy) "
                "(epoch wk{} tow{:.0f}, IOD-SSR {}) to store ({} sats cached)",
                parser.lastMessageType(), n_ssr_pushed, n_ssr_dropped,
                hdr.epoch_time.week, hdr.epoch_time.tow, hdr.iod_ssr, ssr_store.size());
        else
            log_manager_.getLogger("rt_engine")->trace(
                "RTCM {}: pushed {} SSR records (epoch wk{} tow{:.0f}, IOD-SSR {}) "
                "to store ({} sats cached)",
                parser.lastMessageType(), n_ssr_pushed, hdr.epoch_time.week,
                hdr.epoch_time.tow, hdr.iod_ssr, ssr_store.size());
        // obs↔SSR sync: the freshest SSR content just advanced, so a previously
        // held obs epoch may now be covered — re-attempt it.  No-op when nothing
        // is held or the sync feature is off.
        drainHeldObs();
    }
    if (!parser.getBroadcastEphemeris().empty()) {
        log_manager_.getLogger("rt_engine")->trace(
            "RTCM {}: cached {} broadcast ephemeris ({} sats in live store)",
            parser.lastMessageType(), parser.getBroadcastEphemeris().size(),
            eph_store.size());
    }
}

/**
 * @brief RTCM 1005/1006 reference-station ARP -> filter a-priori seed, with the RTA-12(3) blunder gate.
 *
 * Relocated verbatim from the pre-cut RealTimeEngine::feedRtcmFrame
 * (frozen lines 583-686); see scripts/rt_seam_verify.py.
 */
void RealTimeEngine::seedAprioriFromStationCoords(RtcmParser& parser) {
    // Reference-station coordinate (RTCM 1005/1006).  Seed the filter a-priori
    // from the antenna reference point until the KF has bootstrapped — the RTCM
    // analogue of the PVTGeodetic seed in feedSbfFrame.  A live RTCM obs stream
    // carries no RINEX APPROX POSITION, so without this the a-priori is the
    // coarse Earth-centred fallback (geographic pole) from which almost every
    // satellite sits below the elevation mask and the filter can never converge.
    // A configured reference_position always wins; once first_epoch_ clears the
    // KF owns the position and we stop overwriting it.  The startup defer gate
    // keeps first_epoch_ true through warmup, so a 1005/1006 arriving a few
    // seconds into the stream (they come less often than the 1 Hz obs) still
    // seeds before the first estimation epoch.
    if (!parser.getStationCoords().empty()) {
        const auto& s = parser.getStationCoords().front();
        if (first_epoch_ && !config_.hasReferencePosition()
            && s.arp_ecef.allFinite() && s.arp_ecef.norm() > 1e3) {
            // RTA-12(3): blunder gate.  When armed (realtime.arp_blunder_gate_m
            // > 0) and a plausible a-priori already exists (RINEX-header approx
            // position, SBF PVT seed, or an earlier ARP seed), a decoded ARP that
            // disagrees by more than the gate is REJECTED — a mis-configured
            // caster (wrong mountpoint, stale 1005 coordinates) must not teleport
            // the a-priori.  0 (default) keeps the legacy seed-always behaviour.
            const double gate_m = config_.getRealTimeConfig().arp_blunder_gate_m;
            bool blunder = false;
            // RTA-12 fix: only cross-check against a GENUINE prior.  The initial
            // a-priori is the geographic-pole fallback (0,0,EARTH_RADIUS,
            // norm ~6.37e6), which trivially clears norm()>1e3 — testing against
            // it rejects the FIRST legitimate ARP seed (thousands of km from the
            // pole) and the filter never bootstraps.  A prior exists only once
            // ppp_state_.position has moved away from initial_position_ (an SBF
            // PVT seed, an earlier ARP seed, or a RINEX-approx a-priori).
            if (gate_m > 0.0 && ppp_state_.position.allFinite()
                && (ppp_state_.position - initial_position_).norm() > 1e3) {
                const double dist_m = (s.arp_ecef - ppp_state_.position).norm();
                if (dist_m > gate_m) {
                    blunder = true;
                    if (!arp_blunder_warned_) {
                        arp_blunder_warned_ = true;
                        log_manager_.getLogger("rt_engine")->warn(
                            "RTCM {}: station {} broadcast ARP ({:.3f}, {:.3f}, {:.3f}) m "
                            "disagrees with the existing a-priori ({:.3f}, {:.3f}, {:.3f}) m "
                            "by {:.1f} m (> realtime.arp_blunder_gate_m = {:.1f}) — NOT "
                            "overwriting the a-priori (suspect caster/mountpoint "
                            "misconfiguration or stale 1005/1006 coordinates)",
                            parser.lastMessageType(), s.reference_station_id,
                            s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z(),
                            ppp_state_.position.x(), ppp_state_.position.y(),
                            ppp_state_.position.z(), dist_m, gate_m);
                    }
                }
            }
            const size_t pidx = state_manager_->getPositionIndex();
            Eigen::VectorXd sv = state_manager_->getStateVector();
            if (!blunder && sv.size() >= static_cast<Eigen::Index>(pidx + 3)) {
                sv.segment<3>(pidx) = s.arp_ecef;
                state_manager_->setStateVector(sv);
                ppp_state_.position = s.arp_ecef;
                if (!apriori_seeded_from_station_) {
                    log_manager_.getLogger("rt_engine")->info(
                        "RTCM {}: seeded a-priori from station {} ARP "
                        "({:.3f}, {:.3f}, {:.3f}) m",
                        parser.lastMessageType(), s.reference_station_id,
                        s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z());
                    apriori_seeded_from_station_ = true;
                }
            }
        } else {
            // A-priori already pinned (config reference_position) or already
            // owned by the KF — just surface the broadcast ARP so it can be
            // eyeballed against the configured/estimated position.
            // RTA-12(3): with the gate armed, additionally CROSS-CHECK the
            // broadcast ARP against a configured reference position (which is
            // never overwritten regardless) and upgrade to a one-shot WARN on
            // disagreement — the operator pointed the engine at the wrong
            // mountpoint or configured the wrong station.
            const double gate_m = config_.getRealTimeConfig().arp_blunder_gate_m;
            bool flagged = false;
            if (gate_m > 0.0 && config_.hasReferencePosition()
                && s.arp_ecef.allFinite() && s.arp_ecef.norm() > 1e3) {
                const auto& rp = config_.getReferencePositionEcef();
                const Eigen::Vector3d ref(rp[0], rp[1], rp[2]);
                const double dist_m = (s.arp_ecef - ref).norm();
                if (dist_m > gate_m) {
                    flagged = true;
                    if (!arp_blunder_warned_) {
                        arp_blunder_warned_ = true;
                        log_manager_.getLogger("rt_engine")->warn(
                            "RTCM {}: station {} broadcast ARP ({:.3f}, {:.3f}, {:.3f}) m "
                            "disagrees with the configured reference position "
                            "({:.3f}, {:.3f}, {:.3f}) m by {:.1f} m (> "
                            "realtime.arp_blunder_gate_m = {:.1f}) — the configured "
                            "position stays authoritative (check mountpoint/station)",
                            parser.lastMessageType(), s.reference_station_id,
                            s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z(),
                            ref.x(), ref.y(), ref.z(), dist_m, gate_m);
                    }
                }
            }
            if (!flagged)
                log_manager_.getLogger("rt_engine")->info(
                    "RTCM {}: station {} ARP=({:.4f},{:.4f},{:.4f})",
                    parser.lastMessageType(), s.reference_station_id,
                    s.arp_ecef.x(), s.arp_ecef.y(), s.arp_ecef.z());
        }
    }
}

} // namespace GnssProcessor
