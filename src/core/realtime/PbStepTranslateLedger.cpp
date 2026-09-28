/**
 * @file PbStepTranslateLedger.cpp
 * @brief Translate / skip / refuse decision for a phase-bias product step (card
 *        PBT (A)).  The header holds the algebra, the scope and the refusal branches.
 */

#include "realtime/PbStepTranslateLedger.hpp"

#include "filter/StateManager.hpp"

#include <cmath>

namespace GnssProcessor {

const char* PbStepTranslateLedger::actionName(Action a) {
    switch (a) {
        case Action::None:                return "none";
        case Action::TranslateDeclared:   return "translate_disc";
        case Action::TranslateUndeclared: return "translate_undeclared";
        case Action::SkippedEps:          return "translate_skip_eps";
        case Action::ResetFirstSight:     return "reset_first_sight";
        case Action::ResetBasis:          return "reset_basis";
        case Action::ResetBssd:           return "reset_bssd";
        case Action::ResetUnknownKey:     return "reset_unknown_key";
        case Action::RefusedUndeclared:   return "translate_refused";
    }
    return "?";
}

PbStepTranslateLedger::Decision
PbStepTranslateLedger::react(StateManager& sm, const std::string& key,
                             const std::string& amb_key, bool declared, bool applied_now,
                             double applied_now_m, bool basis_nlwl, bool bssd,
                             double eps_m, double reset_std_m) {
    Decision d;
    ++ctr_.cells;
    if (declared) ++ctr_.declared_seen;

    // A declared cell this ledger cannot translate falls back to the existing
    // reset: the same StateManager call resetOnPhaseBiasDisc makes, so the fallback
    // reproduces that reaction exactly.  An undeclared cell is never reset: the
    // reset path does nothing there, and resetting on every such move would add a
    // new, harmful reaction.
    auto refuse = [&](Action why, uint64_t& ctr) {
        if (declared) {
            ++ctr;
            sm.resetAmbiguity(amb_key, reset_std_m);
            d.action = why;
        } else {
            ++ctr_.refused_undeclared;
            d.action = Action::RefusedUndeclared;
        }
        return d;
    };

    // (1) No usable (k−1, k) pair: first sighting or re-admission, or either side
    //     without an applied phase bias (the A5 freshness edge).  There is no Δ for
    //     the ambiguity to absorb; a declared cell takes the existing first-sighting
    //     / flip reset, an undeclared one is only counted.
    const auto prev = applied_.find(key);
    if (prev == applied_.end() || !prev->second.applied || !applied_now) {
        if (declared) {
            ++ctr_.reset_first_sight;
            sm.resetAmbiguity(amb_key, reset_std_m);
            d.action = Action::ResetFirstSight;
        } else {
            ++ctr_.no_pair;
            d.action = Action::None;
        }
        return d;
    }

    // (2) The step itself.
    d.delta_m = applied_now_m - prev->second.value_m;
    if (!(std::abs(d.delta_m) > eps_m)) {
        // Inside the band the filter absorbs the move as noise, and a declared cell
        // whose product did not move (the empty declaration, CST §7.1) costs nothing
        // instead of a reset that starts a new arc.
        ++ctr_.skipped_eps;
        if (declared) ++ctr_.skipped_eps_declared;
        d.action = Action::SkippedEps;
        return d;
    }

    // (3) The structural refusals, in card CC's order.
    if (basis_nlwl) return refuse(Action::ResetBasis, ctr_.reset_basis);
    if (bssd)       return refuse(Action::ResetBssd, ctr_.reset_bssd);
    if (sm.getAmbiguityIndex(amb_key) < 0)
        return refuse(Action::ResetUnknownKey, ctr_.reset_unknown_key);

    // (4) The translation: amb ← amb − Δ, value only, covariance untouched.
    if (!sm.shiftAmbiguity(amb_key, -d.delta_m))
        return refuse(Action::ResetUnknownKey, ctr_.reset_unknown_key);   // defensive; unreachable after the index check
    ctr_.sum_abs_translate_m += std::abs(d.delta_m);
    if (std::abs(d.delta_m) > ctr_.max_abs_translate_m)
        ctr_.max_abs_translate_m = std::abs(d.delta_m);
    if (declared) { ++ctr_.translate_declared;   d.action = Action::TranslateDeclared; }
    else          { ++ctr_.translate_undeclared; d.action = Action::TranslateUndeclared; }
    return d;
}

} // namespace GnssProcessor
