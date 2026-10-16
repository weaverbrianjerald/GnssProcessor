/**
 * @file NetworkRelevel.cpp
 * @brief Implementation of @ref networkEphCutoverRelevel, the ephemeris-cutover re-level
 *        block moved out of @ref networkSolveGlue (NetworkSolveGlue.cpp) at the Card-6
 *        relevel decomposition.  The block body is copied unchanged (dedent 0); the only
 *        added code is the alias preamble, which copies the SolveGlue carry aliases and
 *        the emitEvent forwarder so the body reads the original names.
 */

#include "filter/NetworkRelevel.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"           // systemOfSat
#include "logging/NetworkRunTraces.hpp"
#include "filter/NetworkRunTypes.hpp"        // CarryPrior / CoupledShared / SharedParamKind / ClockOsc
#include "products/LiveEphemerisStore.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

long long ephCutoverShiftCarry(NetworkCarryState& carry, net::NetworkEntities& entities,
                               const std::string& sat, double d,
                               bool cpl_cond, CarrySiteCounts* counts, bool complete) {
    return forEachCarriedReference(
        carry, entities, carryMechanismSiteMask(CarryMechanism::EphRelevel, complete),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::Clock || r.sat != sat) return false;
            switch (r.site) {
                case CarrySite::ClockOsc:
                    if (!r.osc->init) return false;   // the two-state offset (clock_model)
                    break;
                case CarrySite::AmbiguityCoupling:
                case CarrySite::ZwdCoupling:
                // network.carry_rebase_complete (card PRM phase 3b): every other coupling map's
                // clock terms, on the same gate.
                case CarrySite::IonoCoupling:
                case CarrySite::IonoBlockCoupling:
                case CarrySite::HoldAmbiguityCoupling:
                case CarrySite::HoldIonoCoupling:
                case CarrySite::HoldIonoBlockCoupling:
                case CarrySite::RxClockCoupling:
                case CarrySite::RxPhaseBiasCoupling:
                case CarrySite::RxCodeBiasCoupling:
                case CarrySite::PhaseColourCoupling:
                    if (!cpl_cond) return false;      // the coupling stash (card 1)
                    break;
                default:   // the D2 clock carry, the jump baseline; under the knob the stash's
                           // pivot value and the excursion and hold clock baselines
                    break;
            }
            *r.value -= d;
            return true;
        },
        counts);
}

void networkEphCutoverRelevel(NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces,
                              const SyncedEpoch& se,
                              const std::vector<std::string>& registered_clock_sats,
                              bool brdc_rts) {
    // --- Alias preamble (byte-copies of the SolveGlue run-TU alias block). ---
    const bool carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };

// --- Ephemeris-cutover re-level (card 6, RTF-2 / plan D3) ------------------------------
// The producer's estimable per-satellite clock is Δδtˢ = (true satellite clock) −
// (broadcast a-priori clock of the version this epoch's geometry uses).  When that
// broadcast version rolls (old IODE → new IODE), the a-priori jumps by
// Δ_eph(t) = c·(brdc_new(t) − brdc_old(t)); the true clock is continuous, so Δδtˢ must
// jump by −Δ_eph to keep representing the same satellite.  Without a re-base, the
// carried clock prior (the tight two-state clock_osc prediction, or the D2 q_clock
// carry) sits a full Δ_eph out of frame and pulls against the new-frame measurements:
// a slow transient that also trips the two-state NIS / clock-monitor jump test as a
// spurious per-satellite fault.  The block therefore re-bases the carried clock state
// by −Δ_eph, and the solve runs in the new frame.  Emitted C0 stays continuous in
// reconstruction (the consumer forms brdc(new_iode)+C0, card 7), with no
// discontinuity bump.
//
// Ordering: the re-base precedes the prior injection (below) and the solve.  This
// deviates from the plan, which placed it after the solve and before the C1 pass, where
// card 5 stamps clock_iode (after toSolutionEpoch).  After the solve, x_s has been
// estimated against the old-frame prior, so a post-hoc x_s shift is exact only when a
// tight prior dominates, and over-corrects a data-dominated (loose) solve.  Shifting the
// prior is exact for any prior weight; it depends only on the broadcast ephemeris, so it
// is independent of the solve and applies identically on every disposition (emit /
// σ̂0-withhold / solve-fail).  The IODE this epoch uses is read from the shared
// LiveEphemerisStore (card 5's equivalence note: toe granularity ≫ intra-epoch light
// travel, so the query lands on the version the emission path used), so nothing
// depends on the post-solve stamp.
//
// The block sits after the observability gate.  A gate-withheld epoch leaves the carried
// clock state untouched (its post-emit update never runs), so deferring the re-level to
// the next epoch that passes the gate keeps relevel_iode and the carried state in
// lock-step.  On the σ̂0-withhold and solve-fail paths the block has run and the shift
// has landed; the post-emit updates are skipped and leave it in place.  Neither path
// applies the shift twice.
if (brdc_rts) {
    auto& eph_store = LiveEphemerisStore::getInstance();
    for (const std::string& sat : registered_clock_sats) {
        if (sat.empty() || systemOfSat(sat) == System::GLONASS) continue;   // GLONASS out of scope (plan D8)
        const auto now = eph_store.computeSatelliteClock(sat, se.time);
        if (!now) continue;                                  // no eph held → nothing to track
        const int new_iode = now->iode;
        const int* prev = net::getSat(ctx.entities.sats, sat, &net::SatelliteState::relevel_iode);
        if (prev == nullptr) {                               // first sighting: seed, no shift
            net::putSat(ctx.entities, sat, &net::SatelliteState::relevel_iode) = new_iode;
            continue;
        }
        if (*prev == new_iode) continue;                     // no roll (the common case)
        const int old_iode = *prev;
        const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
        net::putSat(ctx.entities, sat, &net::SatelliteState::relevel_iode) = new_iode;   // baseline advances regardless
        if (shift) {
            // Re-base the carried clock state by −Δ_eph.  Only the offset / level term
            // moves; the drift (c1) and the 2×2 covariance are frame-invariant.
            const double d = *shift;
            // Every carried reference of the satellite's clock through the one traversal
            // (card PRM phase 2): the oscillator's offset (clock_model), the D2 clock carry,
            // the clock-monitor jump baseline, and under the conditional coupling the s_prev of
            // the clock terms (card 1).  The coupling rows reference this clock through those
            // terms, and a row's functional u = r + Mᵀs is frame-consistent only when all
            // carried references move together.  The cut-over epoch's own rows were built in
            // PASS B, before this point, on the references before the shift (the structure
            // card PRM section 15.3 records for the handover); every later epoch reads them
            // moved.  The sites the re-level does not move are declared in the mechanism table
            // (card CLV section 6.4 item 6).
            CarrySiteCounts prm_cc;
            ephCutoverShiftCarry(ctx.carry, ctx.entities, sat, d, carry_cpl_cond, &prm_cc,
                                 ctx.knobs.rebase_complete);
            ++ctx.counters.prm_ref_calls[static_cast<int>(CarryMechanism::EphRelevel)];
            carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::EphRelevel, prm_cc);
            log->info("Network (Card 6): {} eph cutover IODE {}->{} — re-leveled carried "
                      "clock by {:.4f} m (−Δ_eph); no discontinuity bump.",
                      sat, old_iode, new_iode, -d);
            emitEvent("RELEVEL", "eph_cutover_relevel", sat, "", -d, std::nullopt,
                      "relevel", fmt::format("iode={}->{}", old_iode, new_iode));
        } else {
            // The prior version is no longer retained, so Δ_eph cannot be computed and
            // the clock stepped by an unknown amount.  Force a per-satellite
            // discontinuity (the consumer resets this satellite's network-corrected
            // ambiguity across the step); NetworkSolPrep lands it at the satellite's next
            // actual emit.  Not expected when the check runs every epoch.
            net::markSat(ctx.entities, sat, net::Disc::EphCutover);
            log->warn("Network (Card 6): {} eph cutover IODE {}->{} but the prior version "
                      "is no longer held — forcing a per-sat discontinuity (Δ_eph "
                      "incomputable).", sat, old_iode, new_iode);
            emitEvent("RELEVEL", "eph_cutover_disc", sat, "", std::nullopt, std::nullopt,
                      "disc", fmt::format("iode={}->{}", old_iode, new_iode));
        }
    }
}
}

} // namespace GnssProcessor
