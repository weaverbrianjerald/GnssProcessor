/**
 * @file NetworkSecondCodeScope.cpp
 * @brief Implementation of the second-code-channel primitives (card SCC).  See
 *        the header for the contract and for which seam consumes each one.
 */
#include "filter/NetworkSecondCodeScope.hpp"

#include <algorithm>
#include <iterator>

namespace GnssProcessor {

bool secondCodeAllowEntryValid(const std::string& entry, std::string* why) {
    const auto refuse = [&](const char* reason) {
        if (why) *why = reason;
        return false;
    };
    if (entry.size() != 3)
        return refuse("not a 3-character RINEX observation code");
    if (entry[0] != 'C')
        return refuse("not a CODE type (must start with 'C')");
    if (entry[1] != '1')
        return refuse("not on band 1 (band 2 is a follow-up card: it would wake "
                      "the untested bridging path of network.sat_isc_state)");
    if (why) why->clear();
    return true;
}

std::vector<std::string> secondCodeCompanions(
    const std::set<std::string>& reported_codes,
    const std::string& selected_code,
    const std::vector<std::string>& allow_list) {

    std::vector<std::string> out;
    // No selected code means there is nothing to be a companion OF: the band has
    // no canonical row this epoch, so any allow-listed code that arrives is the
    // FALLBACK case, handled on the receiver side and counted separately.
    if (selected_code.empty()) return out;
    for (const std::string& sig : allow_list) {
        if (sig == selected_code) continue;                   // that IS the canonical
        if (!reported_codes.count(sig)) continue;             // the station does not track it
        if (std::find(out.begin(), out.end(), sig) != out.end()) continue;  // duplicate entry
        out.push_back(sig);
    }
    return out;
}

void secondCodeCellEdges(const std::set<std::string>& prev,
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

}  // namespace GnssProcessor
