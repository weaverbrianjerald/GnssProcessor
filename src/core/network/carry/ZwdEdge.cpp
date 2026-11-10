// The wet delay's edge record on the float vehicle (network/carry/ZwdEdge.hpp).

#include "network/carry/ZwdEdge.hpp"

#include <utility>

namespace GnssProcessor {

const char* zwdEdgeCauseName(ZwdEdgeCause c) {
    switch (c) {
        case ZwdEdgeCause::FirstSighting: return "first_sighting";
        case ZwdEdgeCause::SlipLatch:     return "slip_latch";
        case ZwdEdgeCause::RaimArc:       return "raim_attribution";
        case ZwdEdgeCause::RaimPrior:     return "raim_prior";
        case ZwdEdgeCause::GapPrune:      return "gap_prune";
        case ZwdEdgeCause::Return:        return "return_in_horizon";
    }
    return "?";
}

std::optional<ZwdEdgeCause> zwdEdgeDecouple(std::optional<ZwdCoupleEdge>& edge, ZwdEdgeCause cause,
                                            double now_s, std::string sat, std::string via) {
    // A record whose entry was re-created and has not folded yet loses its re-coupling.
    std::optional<ZwdEdgeCause> superseded;
    if (edge && edge->created_s > 0.0) superseded = edge->cause;
    edge = ZwdCoupleEdge{cause, now_s, 0.0, std::move(sat), std::move(via)};
    return superseded;
}

void zwdEdgeCreate(std::optional<ZwdCoupleEdge>& edge, double now_s) {
    // No open record: the station has not carried a wet delay on this run before.
    if (!edge) edge = ZwdCoupleEdge{ZwdEdgeCause::FirstSighting, now_s, 0.0, {}, {}};
    edge->created_s = now_s;
}

std::optional<ZwdRecouple> zwdEdgeFold(std::optional<ZwdCoupleEdge>& edge, bool folded,
                                       double gap_s, double dt_s, double now_s) {
    if (edge) {
        // The record stays open until the re-created entry actually folds: an epoch that did
        // not fold it (no block, a gap refusal) is not its re-coupling.
        if (edge->created_s <= 0.0 || !folded) return std::nullopt;
        ZwdRecouple r;
        r.cause  = edge->cause;
        r.away_s = edge->cause == ZwdEdgeCause::FirstSighting ? 0.0 : now_s - edge->decoupled_s;
        r.sat    = std::move(edge->sat);
        r.via    = std::move(edge->via);
        edge.reset();
        return r;
    }
    // A kept entry that folds after more than one epoch without a stash: the station came back
    // within the horizon (or its harvest skipped an epoch), and the fold re-couples it.
    if (folded && dt_s > 0.0 && gap_s > 1.5 * dt_s) {
        ZwdRecouple r;
        r.cause  = ZwdEdgeCause::Return;
        r.away_s = gap_s;
        return r;
    }
    return std::nullopt;
}

} // namespace GnssProcessor
