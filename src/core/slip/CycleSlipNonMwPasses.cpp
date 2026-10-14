/**
 * @file CycleSlipNonMwPasses.cpp
 * @brief Sections S15 and S16 of @c CycleSlipManager::detectAndRepair: the
 *        evidence that is not an MW mandate, applied to the satellites the MW
 *        path did not handle (TurboEdit GF-only resolutions, then the
 *        auxiliary detectors' resets).
 *
 * Bodies moved verbatim from CycleSlipManager.cpp (card UOM step U3;
 * scripts/uom_u3_verify.py); each pass's guard moved from the driver into the
 * pass.
 */

#include "slip/CycleSlipManager.hpp"
#include "slip/CycleSlipEpochWork.hpp"
#include "slip/SlipDetectorRegistry.hpp"

namespace GnssProcessor {

// ===========================================================================
// TurboEdit GF-only (MW blind-spot) slip handling
// ===========================================================================

void CycleSlipManager::applyTurboEditGfOnlyResolutions(SlipEpochWork& w) {
    const auto& te            = w.te;
    auto& handled_sats        = w.reset_sats;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    const auto& epoch_prefix  = w.epoch_prefix;
    auto& result              = w.result;

    // --- TurboEdit GF-only detections (MW dN1==dN2 blind spot) ---
    // The dual-frequency MW-flagged case was already resolved inside the MW
    // per-satellite loop above (routed through the BSSD pivot-deferral
    // machinery).  This pass applies TurboEdit's remaining resolutions for
    // satellites the MW detector reported nothing on: slips where the
    // wide-lane is unchanged and only the geometry-free jump moved.  Satellites
    // already handled this epoch (in reset_sats) are skipped.
    if (!turbo_) return;

    auto log = log_manager_.getLogger("cycle_slip_manager");

    // Group confident per-signal repairs by satellite so all of a satellite's
    // signals are applied before it is marked handled.
    std::map<std::string, std::vector<const TurboEditResolver::SignalRepair*>> by_sat;
    for (const auto& r : te.repairs) by_sat[r.satellite].push_back(&r);

    for (const auto& [sat, reps] : by_sat) {
        if (handled_sats.count(sat)) continue;  // MW pipeline already acted on this sat
        bool any = false;
        for (const auto* r : reps) {
            const std::string key = sat + "_" + r->signal;
            if (state_manager.getAmbiguityIndex(key) < 0) continue;
            // Covariance-preserving additive integer repair; repairAmbiguityBssdAware
            // refuses the BSSD pivot (and returns false when the sat is unobserved
            // or λ is invalid); the ambiguity is then reset instead.
            const bool ok = repairAmbiguityBssdAware(
                sat, r->signal, r->dN, modeled_obs, state_manager, meas_model, epoch_prefix);
            if (ok) {
                ++result.n_repaired;
                log->info("{}TurboEdit GF-only REPAIR {} {}: dN={:+d} cyc (MW blind spot; "
                          "covariance preserved)", epoch_prefix, sat, r->signal, r->dN);
            } else {
                resetAmbiguityBssdAware(key, sat, r->signal, modeled_obs,
                                        state_manager, meas_model, epoch_prefix);
                ++result.n_reset;
                log->info("{}TurboEdit GF-only repair refused for {} {} (pivot / unobserved); "
                          "reset instead", epoch_prefix, sat, r->signal);
            }
            any = true;
        }
        if (any) { result.state_modified = true; handled_sats.insert(sat); }
    }

    // Satellites the GF jump flagged but could not resolve confidently (iono
    // disturbed, non-integer split): reset every observed phase ambiguity.
    for (const auto& sat : te.reset_sats) {
        if (handled_sats.count(sat)) continue;
        const PppObservation* obs = nullptr;
        for (const auto& o : modeled_obs)
            if (o.satellite == sat) { obs = &o; break; }
        if (!obs) continue;
        bool any = false;
        for (const auto& [sig, val] : obs->observed) {
            (void)val;
            if (sig.empty() || sig[0] != 'L') continue;          // phase only
            if (sig.find('_') != std::string::npos) continue;     // raw signal only
            const std::string key = sat + "_" + sig;
            if (state_manager.getAmbiguityIndex(key) < 0) continue;
            resetAmbiguityBssdAware(key, sat, sig, modeled_obs,
                                    state_manager, meas_model, epoch_prefix);
            ++result.n_reset;
            any = true;
        }
        if (any) {
            result.state_modified = true;
            handled_sats.insert(sat);
            log->info("{}TurboEdit GF-only reset for {} (GF jump unresolved — "
                      "iono disturbed or non-integer split)", epoch_prefix, sat);
        }
    }
}

// ===========================================================================
// Auxiliary (non-MW) detector slip handling
// ===========================================================================

void CycleSlipManager::applyAuxiliarySlipEvents(SlipEpochWork& w) {
    const auto& events        = w.aux_events;
    auto& handled_sats        = w.reset_sats;
    const auto& modeled_obs   = w.modeled_obs;
    auto& state_manager       = w.state_manager;
    const auto& meas_model    = w.meas_model;
    const auto& epoch_prefix  = w.epoch_prefix;
    auto& result              = w.result;

    // --- Auxiliary detectors (GRAPHIC / EWL / ...): reset on detection ---
    if (events.empty()) return;

    auto log = log_manager_.getLogger("cycle_slip_manager");

    for (const auto& ev : events) {
        const std::string sat = ev.satellite.empty()
            ? satIdFromAmbiguityKey(ev.amb_key) : ev.satellite;
        if (sat.empty()) continue;

        // Skip satellites the MW pipeline already handled this epoch: its
        // targeted, covariance-preserving per-signal repairs take precedence,
        // and an auxiliary reset on top would discard that precision.
        if (handled_sats.count(sat)) continue;

        auto resetSignal = [&](const std::string& signal) {
            if (signal.empty()) return;
            const std::string key = sat + "_" + signal;
            if (state_manager.getAmbiguityIndex(key) < 0) return;
            resetAmbiguityBssdAware(key, sat, signal, modeled_obs,
                                    state_manager, meas_model, epoch_prefix);
            ++result.n_reset;
            result.state_modified = true;
            log->info("{}{} slip: BSSD-aware reset {} (auxiliary detector)",
                      epoch_prefix, toString(ev.source), key);
        };

        if (ev.granularity == SlipGranularity::BandDifference) {
            // A band-difference detector (EWL) cannot, on its own, attribute
            // the jump to a single signal, so both members of the pair are reset.
            resetSignal(ev.sig1);
            resetSignal(ev.sig2);
        } else {
            resetSignal(ev.sig1.empty()
                            ? signalFromAmbiguityKey(ev.amb_key) : ev.sig1);
        }

        handled_sats.insert(sat);  // de-dupe across multiple auxiliary detectors
    }
}

} // namespace GnssProcessor
