/**
 * @file NetworkExtraBandScope.cpp
 * @brief Implementation of the extra-band primitive (card L5P).  See the header
 *        for the contract and for which seam consumes each predicate.
 */
#include "filter/NetworkExtraBandScope.hpp"

#include "common/GnssIdentity.hpp"
#include "products/ProductCache.hpp"

namespace GnssProcessor {

NetworkPinBands networkPinBandsFor(char system_char) {
    NetworkPinBands out;
    if (system_char == '\0') return out;
    const auto& refs = ProductCache::getInstance().getClockRefSignals();
    const auto it = refs.find(system_char);
    if (it == refs.end()) return out;
    const auto& ref = it->second;
    const System sys = systemFromLetter(system_char);
    const auto sig1 = SignalId::fromObsCode(sys, ref.sig1);
    const auto sig2 = SignalId::fromObsCode(sys, ref.sig2);
    if (!sig1 || !sig2) return out;
    out.band1 = sig1->digit;
    out.band2 = sig2->digit;
    // A degenerate pair (same band twice, or a malformed code) is reported as
    // invalid, not half-resolved: every consumer gates on valid().
    return out;
}

bool networkSignalIsExtraBand(const std::string& signal, const NetworkPinBands& pin) {
    if (!pin.valid()) return false;           // unknown pin pair: nothing is extra
    if (signal.size() < 2) return false;      // malformed code: never acted on
    const char band = signal[1];
    return band != pin.band1 && band != pin.band2;
}

bool networkMwPairInPrimaryScope(const std::string& sig1, const std::string& sig2,
                                 const NetworkPinBands& pin) {
    if (!pin.valid()) return true;            // unknown pin pair: form every pair
    if (sig1.size() < 2 || sig2.size() < 2) return true;
    const char b1 = sig1[1];
    const char b2 = sig2[1];
    const auto on_pin = [&](char b) { return b == pin.band1 || b == pin.band2; };
    return on_pin(b1) && on_pin(b2);
}

}  // namespace GnssProcessor
