/**
 * @file NetworkAnchorReentry.cpp
 * @brief Anchor re-entry against the arc ledger (card SID phase 4, S2 and B1); see
 *        NetworkAnchorReentry.hpp.
 *
 * Every function here is a pure function of the ledger and the run-scope memory: no
 * context, no config, no estimator.  The election remains the only scope that knows
 * about traces, counters and the pin, as with the card EV7 drop-edge predicate
 * (@c NetworkPhaseGateDrop.hpp) and its own rule.
 */
#include "filter/NetworkAnchorReentry.hpp"

namespace GnssProcessor {

const char* anchorReentryClassName(AnchorReentryClass c) {
    switch (c) {
    case AnchorReentryClass::NoMemory:       return "no_memory";
    case AnchorReentryClass::SameGeneration: return "same_generation";
    case AnchorReentryClass::NewGeneration:  return "new_generation";
    }
    return "?";
}

void anchorReentryFamilyCensus(const ArcLedger& led, const std::string& sat,
                               const std::string& signal,
                               int& n_tracking, int& n_gap, int& n_dead,
                               std::map<int, int>* gen) {
    n_tracking = n_gap = n_dead = 0;
    if (gen) gen->clear();
    for (const auto& [key, a] : led.arcs()) {
        if (std::get<1>(key) != sat || std::get<2>(key) != signal) continue;
        // The admitted side throughout: the carried ambiguity, the leveling level
        // and the phase-bias column are admitted-side objects, so a raw-side state
        // would answer a different question than the one the election is asking.
        switch (a.adm_state) {
        case ArcState::Tracking:
            ++n_tracking;
            if (gen) (*gen)[std::get<0>(key)] = a.adm_generation;
            break;
        case ArcState::Reentered:
            ++n_gap;
            if (gen) (*gen)[std::get<0>(key)] = a.adm_generation;
            break;
        case ArcState::Gap:  ++n_gap;  break;
        case ArcState::Dead: ++n_dead; break;
        case ArcState::Absent: break;
        }
    }
}

AnchorReentryClass anchorReentryClassify(const ArcLedger& led,
                                         const AnchorDropMemo& memo,
                                         const std::string& sat,
                                         const std::string& signal,
                                         const std::map<std::string, int>& sta_index,
                                         int* out_station) {
    if (out_station) *out_station = -1;
    if (memo.ep < 0 || memo.prev_anchor.empty()) return AnchorReentryClass::NoMemory;
    const auto si = sta_index.find(memo.prev_anchor);
    if (si == sta_index.end()) return AnchorReentryClass::NewGeneration;
    const auto gi = memo.generation.find(si->second);
    if (gi == memo.generation.end()) return AnchorReentryClass::NewGeneration;
    const ArcRecord* a = led.find(ArcLedger::Key{ si->second, sat, signal });
    if (a == nullptr) return AnchorReentryClass::NewGeneration;
    if (a->adm_state != ArcState::Tracking && a->adm_state != ArcState::Reentered)
        return AnchorReentryClass::NewGeneration;
    if (a->adm_generation != gi->second) return AnchorReentryClass::NewGeneration;
    if (out_station) *out_station = si->second;
    return AnchorReentryClass::SameGeneration;
}

void anchorReentryNoteDrop(AnchorReentryState& st, const ArcLedger& led,
                           const std::string& amb, const std::string& sat,
                           const std::string& signal,
                           const std::string& prev_anchor, int n_trackers,
                           long long now_ep) {
    AnchorDropMemo m;
    m.prev_anchor = prev_anchor;
    m.ep          = now_ep;
    m.n_trackers  = n_trackers;
    m.returned    = false;
    anchorReentryFamilyCensus(led, sat, signal, m.n_tracking, m.n_gap, m.n_dead,
                              &m.generation);
    // A second drop of the same family replaces the memo rather than merging: a
    // return is judged against the last era the family was anchored in, and
    // merging two eras would let an arc that died between them read as continuous.
    st.drop[amb] = std::move(m);
}

long long anchorLedgerArcAge(const ArcLedger& led, const ArcLedger::Key& key,
                             long long now_ep) {
    const ArcRecord* a = led.find(key);
    if (a == nullptr) return -1;
    if (a->adm_state != ArcState::Tracking && a->adm_state != ArcState::Reentered)
        return -1;
    if (a->ep_adm_state_changed < 0) return -1;
    const long long age = now_ep - a->ep_adm_state_changed;
    return age < 0 ? -1 : age;
}

} // namespace GnssProcessor
