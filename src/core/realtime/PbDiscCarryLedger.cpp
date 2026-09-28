/**
 * @file PbDiscCarryLedger.cpp
 * @brief Carry-vs-reset decision for a declared phase-bias step (card CC, A).
 *        The header holds the algebra and the refusal branches.
 */

#include "realtime/PbDiscCarryLedger.hpp"

#include "filter/StateManager.hpp"

namespace GnssProcessor {

const char* PbDiscCarryLedger::actionName(Action a) {
    switch (a) {
        case Action::None:            return "none";
        case Action::CarriedDisc:     return "carried_disc";
        case Action::CarriedFlip:     return "carried_flip";
        case Action::ResetFirstSight: return "reset_first_sight";
        case Action::ResetBasis:      return "reset_basis";
        case Action::ResetBssd:       return "reset_bssd";
        case Action::ResetUnknownKey: return "reset_unknown_key";
    }
    return "?";
}

PbDiscCarryLedger::Decision PbDiscCarryLedger::react(StateManager& sm, const std::string& key,
                                                     const std::string& amb_key, bool bump,
                                                     bool flip, double applied_now_m,
                                                     bool basis_nlwl, bool bssd,
                                                     double reset_std_m) {
    Decision d;
    if (bump) ++ctr_.disc_bumps;
    if (flip) ++ctr_.flips;
    if (!bump && !flip) return d;
    ++ctr_.events;

    // The refusal ladder.  Every branch takes the existing reset, the same
    // StateManager call the reset reaction (resetOnPhaseBiasDisc) makes, so the
    // fallback is byte-identical to that path (resetAmbiguity itself refuses an
    // unknown key with a debug line, as it does there).
    auto refuse = [&](Action why, uint64_t& ctr) {
        ++ctr;
        sm.resetAmbiguity(amb_key, reset_std_m);
        d.action = why;
        return d;
    };

    const auto prev = applied_m_.find(key);
    if (prev == applied_m_.end())
        return refuse(Action::ResetFirstSight, ctr_.reset_first_sight);
    if (basis_nlwl)
        return refuse(Action::ResetBasis, ctr_.reset_basis);
    if (bssd)
        return refuse(Action::ResetBssd, ctr_.reset_bssd);
    if (sm.getAmbiguityIndex(amb_key) < 0)
        return refuse(Action::ResetUnknownKey, ctr_.reset_unknown_key);

    // The carry: amb_new = amb − Δ, Δ = applied_now − applied_prev.  A bump and
    // a flip in the same epoch are one translation, since Δ already spans both.
    d.delta_m = applied_now_m - prev->second;
    if (!sm.shiftAmbiguity(amb_key, -d.delta_m))
        return refuse(Action::ResetUnknownKey, ctr_.reset_unknown_key);   // unreachable after the index check; defensive
    if (bump) { ++ctr_.disc_carried; d.action = Action::CarriedDisc; }
    else      { ++ctr_.flip_carried; d.action = Action::CarriedFlip; }
    return d;
}

} // namespace GnssProcessor
