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

long long ephCutoverShiftCarry(NetworkCarryState& carry, const std::string& sat, double d,
                               bool cpl_cond, bool dry, CarrySiteCounts* counts) {
    return forEachCarriedReference(
        carry, carryMechanismSites(CarryMechanism::EphRelevel),
        [&](const CarriedRef& r) -> bool {
            if (r.kind != SharedParamKind::Clock || r.sat != sat) return false;
            switch (r.site) {
                case CarrySite::ClockOsc:
                    if (!r.osc->init) return false;   // the two-state offset (clock_model)
                    break;
                case CarrySite::AmbiguityCoupling:
                case CarrySite::ZwdCoupling:
                    if (!cpl_cond) return false;      // the coupling stash (card 1)
                    break;
                default:                              // the D2 clock carry, the jump baseline
                    break;
            }
            if (!dry) *r.value -= d;
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
    auto& relevel_iode = ctx.carry.relevel_iode;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_clk = ctx.carry.prev_clk;
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
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
        if (sat.empty() || sat[0] == 'R') continue;          // GLONASS out of scope (plan D8)
        const auto now = eph_store.computeSatelliteClock(sat, se.time);
        if (!now) continue;                                  // no eph held → nothing to track
        const int new_iode = now->iode;
        auto bit = relevel_iode.find(sat);
        if (bit == relevel_iode.end()) {                     // first sighting: seed, no shift
            relevel_iode[sat] = new_iode;
            continue;
        }
        if (bit->second == new_iode) continue;               // no roll (the common case)
        const int old_iode = bit->second;
        const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
        relevel_iode[sat] = new_iode;                        // baseline advances regardless
        if (shift) {
            // Re-base the carried clock state by −Δ_eph.  Only the offset / level term
            // moves; the drift (c1) and the 2×2 covariance are frame-invariant.
            const double d = *shift;
            // Card PRM phase 2, step 1: the traversal's selection counted beside the
            // hand-written walk (dry: nothing written), and the walk's own moves counted.
            CarrySiteCounts prm_cc;
            const long long prm_n = ephCutoverShiftCarry(ctx.carry, sat, d, carry_cpl_cond,
                                                         /*dry=*/true, &prm_cc);
            long long hand_n = 0;
            if (auto oit = clock_osc.find(sat); oit != clock_osc.end() && oit->second.init) {
                oit->second.x0 -= d;                          // two-state offset (clock_model)
                ++hand_n;
            }
            for (CarryPrior& cp : prev_carry)                 // scalar clock carry (D2, model off)
                if (cp.kind == SharedParamKind::Clock && cp.sat == sat) { cp.value -= d; ++hand_n; }
            if (auto jit = prev_clk.find(sat); jit != prev_clk.end()) {
                jit->second.first -= d;                       // clock-monitor jump baseline
                ++hand_n;
            }
            // The coupling stash references this clock through ŝ_prev terms (card 1).
            // Re-level them into the new frame as well; otherwise every coupling row's
            // RHS is off by M·Δ_eph after a cutover (the row's functional u = r + Mᵀs
            // is frame-consistent only when all carried references move together).
            if (carry_cpl_cond) {
                for (auto& [lk, lcv] : local_carry)
                    for (CoupledShared& cs2 : lcv.coupling)
                        if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                            { cs2.s_prev -= d; ++hand_n; }
                for (auto& [zk, zcv] : zwd_carry)
                    for (CoupledShared& cs2 : zcv.coupling)
                        if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                            { cs2.s_prev -= d; ++hand_n; }
            }
            {
                const int m = static_cast<int>(CarryMechanism::EphRelevel);
                ++ctx.counters.prm_ref_calls[m];
                ++ctx.counters.prm_ref_check[m];
                if (prm_n != hand_n) ++ctx.counters.prm_ref_check_mismatch[m];
                carryTallyMoved(ctx.counters.prm_ref_moved, CarryMechanism::EphRelevel, prm_cc);
            }
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
            eph_cutover_disc_pending.insert(sat);
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
