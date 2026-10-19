/**
 * @file ArcLife.cpp
 * @brief The life stamp of one band-tagged arc, its change mask and the search's track of a
 *        life (step AGT-5).  What each field records and why the next search reads
 *        an echo are in the header's file banner, include/network/ar/ArcLife.hpp.
 */

#include "network/ar/ArcLife.hpp"

#include "network/state/RunContext.hpp"     // ctx.arc_ledger, ctx.entities.arcs (the leveling carry), the station table (amb_flag_ep)
#include "network/state/ArcLedger.hpp"          // ArcRecord, ArcState
#include "network/driver/RunTypes.hpp"    // LocalCarry
#include "network/state/Entities.hpp"     // net::ambKey

#include <string>
#include <tuple>
#include <utility>

namespace GnssProcessor {

namespace {

/// The fields a search may read as the echo of the life the last search began: the ledger
/// catching up with the run (both ledger fields) and the harvest stashing the new carry.
constexpr unsigned kEchoFields = kArcLifeAdmGeneration | kArcLifeAdmRunStart | kArcLifeHasCarry
                               | kArcLifeBirthEp | kArcLifeRelevelS;

/// The admitted run start of a ledger record: the epoch its current unbroken run began.  The
/// ledger enters Tracking at a birth (that epoch) and one epoch after a re-entry (Reentered
/// at the return epoch R, Tracking at R + 1), so both states read R + 1 for a re-entry and
/// the stamp changes once per run.  -1 while the arc is not live.
long long admittedRunStart(const ArcRecord& r) {
    switch (r.adm_state) {
    case ArcState::Tracking:  return r.ep_adm_state_changed;
    case ArcState::Reentered: return r.ep_adm_state_changed + 1;
    default:                  return -1;
    }
}

}  // namespace

CarryLife carryLifeOf(const LocalCarry& lc) {
    CarryLife life;
    life.birth_ep   = lc.birth_ep;
    life.birth_s    = lc.birth_s;
    life.dropped_ep = lc.dropped_ep;
    life.relevel_s  = lc.relevel_s;
    return life;
}

bool operator==(const ArcLifeStamp& a, const ArcLifeStamp& b) {
    return arcLifeChange(a, b) == 0u;
}

bool operator!=(const ArcLifeStamp& a, const ArcLifeStamp& b) { return !(a == b); }

const char* arcLifeFieldName(int k) {
    static constexpr const char* kNames[kArcLifeFieldCount] = {
        "adm_generation", "adm_run_start", "has_carry", "birth_ep", "dropped_ep", "relevel_s",
        "flag_ep"};
    return (k >= 0 && k < kArcLifeFieldCount) ? kNames[k] : "?";
}

ArcLifeStamp arcLifeStampOf(const ArcRecord* ledger, const LocalCarry* carry, const long long* flag_ep) {
    ArcLifeStamp s;
    if (ledger) {
        s.adm_generation = ledger->adm_generation;
        s.adm_run_start  = admittedRunStart(*ledger);
    }
    if (carry) {
        const CarryLife life = carryLifeOf(*carry);
        s.has_carry  = true;
        s.birth_ep   = life.birth_ep;
        s.dropped_ep = life.dropped_ep;
        s.relevel_s  = life.relevel_s;
    }
    if (flag_ep) s.flag_ep = *flag_ep;
    return s;
}

ArcLifeKeys arcLifeKeys(const ArcId& arc) {
    const int         sta = arc.station.index;
    const std::string sat = arc.sat.str();
    const std::string sig = arc.signal.obsCode(ObsType::Phase);
    return ArcLifeKeys{std::make_tuple(sta, sat, sig), {sta, net::ambKey(SatSignalId{arc.sat, arc.signal})}};
}

ArcLifeStamp arcLifeStamp(const NetworkRunContext& ctx, const ArcLifeKeys& keys) {
    // One row (step N4f2): the arc's ledger record and its leveling carry.
    const std::optional<ArcId> id = net::arcKey(keys.arc);
    const auto it = id ? ctx.entities.arcs.find(*id) : ctx.entities.arcs.end();
    const net::ArcState* row = (it == ctx.entities.arcs.end()) ? nullptr : &it->second;
    const ArcRecord*  ledger = (row && row->ledger) ? &*row->ledger : nullptr;
    const LocalCarry* lc     = (row && row->amb) ? &*row->amb : nullptr;
    const long long* fl = net::getAmbFlagEp(ctx.entities.stations, keys.amb.first, keys.amb.second);
    return arcLifeStampOf(ledger, lc, fl);
}

ArcLifeStamp arcLifeStamp(const NetworkRunContext& ctx, const ArcId& arc) {
    return arcLifeStamp(ctx, arcLifeKeys(arc));
}

unsigned arcLifeChange(const ArcLifeStamp& before, const ArcLifeStamp& now) {
    unsigned m = 0u;
    if (before.adm_generation != now.adm_generation) m |= kArcLifeAdmGeneration;
    if (before.adm_run_start != now.adm_run_start)   m |= kArcLifeAdmRunStart;
    if (before.has_carry != now.has_carry)           m |= kArcLifeHasCarry;
    if (before.birth_ep != now.birth_ep)             m |= kArcLifeBirthEp;
    if (before.dropped_ep != now.dropped_ep)         m |= kArcLifeDroppedEp;
    if (before.relevel_s != now.relevel_s)           m |= kArcLifeRelevelS;
    if (before.flag_ep != now.flag_ep)               m |= kArcLifeFlagEp;
    return m;
}

ArcLifeStep arcLifeStep(const ArcLifeTrack* last, long long last_search_ep, const ArcLifeStamp& now,
                        bool freed, long long now_ep) {
    ArcLifeStep step;
    step.track.stamp = now;
    if (!last) {
        step.edge             = ArcLifeEdge::FirstSeen;
        step.track.life_start = now_ep;
        return step;
    }
    step.change = arcLifeChange(last->stamp, now);
    if (freed) {
        step.edge             = ArcLifeEdge::Freed;
        step.track.life_start = now_ep;
        return step;
    }
    step.track.life_start = last->life_start;
    if (step.change == 0u) return step;
    const bool last_began   = last->life_start == last_search_ep;
    const bool carry_erased = last->stamp.has_carry && !now.has_carry;
    if (last_began && (step.change & ~kEchoFields) == 0u && !carry_erased) {
        step.edge = ArcLifeEdge::Echo;
        return step;
    }
    step.edge             = ArcLifeEdge::StampChange;
    step.track.life_start = now_ep;
    return step;
}

}  // namespace GnssProcessor
