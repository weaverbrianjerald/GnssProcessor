/**
 * @file NetworkSecondPhaseScope.cpp
 * @brief Implementation of the second-phase-channel primitives and of the epoch
 *        tail (card SID phase 6a).  See the header for the contract and for which
 *        seam consumes each one.
 */
#include "filter/NetworkSecondPhaseScope.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace GnssProcessor {

bool secondPhaseAllowEntryValid(const std::string& entry, std::string* why) {
    const auto refuse = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (entry.size() != 3)
        return refuse("not a 3-character RINEX observation code");
    if (entry[0] != 'L')
        return refuse("not a PHASE type (must start with 'L'); a code type belongs "
                      "to card SCC");
    if (entry[1] != '1')
        return refuse("not on band 1 (band 2 belongs to phase 6b, which lifts the "
                      "channel_mode interlock instead of working around it)");
    if (why) why->clear();
    return true;
}

std::vector<std::string> secondPhaseCompanions(
    const std::set<std::string>& reported_phases,
    const std::string& selected_phase,
    const std::vector<std::string>& allow_list) {

    std::vector<std::string> out;
    // No latched phase this epoch means there is nothing to be SECOND to: the band
    // has no canonical row, so an allow-listed phase that arrives is the FALLBACK
    // case, which the caller counts and does not add.
    if (selected_phase.empty()) return out;
    for (const std::string& sig : allow_list) {
        if (sig == selected_phase) continue;                  // that IS the canonical
        if (!reported_phases.count(sig)) continue;            // the station does not track it
        if (std::find(out.begin(), out.end(), sig) != out.end()) continue;  // duplicate entry
        out.push_back(sig);
    }
    return out;
}

void secondPhaseCellEdges(const std::set<std::string>& prev,
                          const std::set<std::string>& cur,
                          std::vector<std::string>* opened,
                          std::vector<std::string>* closed) {
    if (opened) {
        opened->clear();
        std::set_difference(cur.begin(), cur.end(), prev.begin(), prev.end(),
                            std::back_inserter(*opened));
    }
    if (closed) {
        closed->clear();
        std::set_difference(prev.begin(), prev.end(), cur.begin(), cur.end(),
                            std::back_inserter(*closed));
    }
}

double secondPhaseWitness(double latched_cyc, double second_cyc) {
    return latched_cyc - second_cyc;
}

void networkSecondPhaseEpochTail(NetworkRunContext& ctx, const GnssConfig& config_,
                                 const std::vector<NetStation>& stations) {
    // --- alias preamble (the seam contract) -------------------------------------
    if (!config_.networkSecondPhaseRowsActive()) return;   // unarmed: one flag test
    auto& C = ctx.counters.second_phase;

    // (1) THE SLIP CENSUS BY TIER (R4, standing invariant #1).  `relevel.free_cause`
    //     is the producer's own per-arc tier bitmask: it is cleared at the TOP of the
    //     next epoch's slip scan, so at this point it holds THIS epoch's causes from
    //     every tier, the post-solve RAIM ones included.  A second-phase arc is one
    //     whose (satellite, signal) cell is open at that station this epoch.
    for (const auto& [key, bits] : ctx.carry.relevel.free_cause) {
        const int sta = key.first;
        if (sta < 0 || sta >= static_cast<int>(stations.size())) continue;
        const auto parts = StateKeys::parseAmbiguityKey(key.second);
        if (!parts) continue;
        if (!stations[static_cast<std::size_t>(sta)].spc_cells.count(
                StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal)))
            continue;
        ++C.slip_arcs;
        if (bits & RelevelEpochState::kCauseMw)        ++C.slip_mw;
        if (bits & RelevelEpochState::kCauseLli)       ++C.slip_lli;
        if (bits & RelevelEpochState::kCauseIfTd)      ++C.slip_iftd;
        if (bits & RelevelEpochState::kCauseT4Attrib)  ++C.slip_t4;
        if (bits & RelevelEpochState::kCauseT4Consens) ++C.slip_t3;
        if (bits & RelevelEpochState::kCauseWindup)    ++C.slip_windup;
        if (bits & RelevelEpochState::kCauseGap)       ++C.slip_gap;
    }

    // (2) THE ANCHOR CENSUS (R3).  A second-phase arc is a phase-datum candidate like
    //     any other, so the question the card owes an answer to is whether the
    //     election ever SEATS one.  `anchor_incumbent` is keyed by the ambiguity form
    //     and holds the incumbent station's NAME.
    for (const auto& [amb, name] : ctx.carry.anchor_incumbent) {
        const auto parts = StateKeys::parseAmbiguityKey(amb);
        if (!parts) continue;
        const std::string cell =
            StateKeys::makeSatPhaseBiasKey(parts->satellite, parts->signal);
        for (const auto& st : stations) {
            if (st.name != name) continue;
            if (st.spc_cells.count(cell)) ++C.anchors;
            break;
        }
    }
}

}  // namespace GnssProcessor
