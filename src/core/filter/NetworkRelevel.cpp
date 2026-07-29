/**
 * @file NetworkRelevel.cpp
 * @brief Implementation of @ref networkEphCutoverRelevel — the Card-6 ephemeris-cutover
 *        re-level block relocated VERBATIM out of @ref networkSolveGlue
 *        (NetworkSolveGlue.cpp) at the Card-6 relevel decomposition (2026-07-23).  The
 *        block body is a byte-copy (dedent-0); the only added code is the alias preamble
 *        that byte-copies the SolveGlue carry aliases + the emitEvent forwarder so the
 *        body reads the original names.
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

// --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level ------------------------------
// The producer's estimable per-sat clock is Δδtˢ = (true sat clock) − (broadcast
// a-priori clock of the version this epoch's geometry uses).  When that broadcast
// version rolls (old IODE → new IODE) the a-priori jumps by
// Δ_eph(t) = c·(brdc_new(t) − brdc_old(t)); the TRUE clock is continuous, so the
// estimable Δδtˢ must jump by −Δ_eph to keep representing the same satellite.  Left
// alone, the carried clock prior — the TIGHT two-state clock_osc prediction, or the
// D2 q_clock carry — is a full Δ_eph out of frame and FIGHTS the new-frame
// measurements: a slow transient that also trips the two-state NIS / clock-monitor
// jump test as a spurious per-sat fault.  So we deterministically re-base the CARRIED
// clock state by −Δ_eph, and the solve runs entirely in the new frame — emitted C0 is
// continuous-in-reconstruction (the consumer forms brdc(new_iode)+C0, Card 7) with NO
// discontinuity bump.
//
// ORDERING (deliberate deviation from the plan's "after the solve, before the C1
// pass"): the re-base MUST precede the prior injection (below) and the solve.  The
// plan's post-solve placement anchored to where Card 5 STAMPS clock_iode (after
// toSolutionEpoch); but by then x_s was already solved against the OLD-frame prior, so
// a post-hoc x_s shift is exact ONLY when the prior dominates (tight) and OVER-corrects
// a data-dominated (loose) solve.  Shifting the PRIOR is unconditionally exact for any
// prior weight AND — being a deterministic function of the broadcast ephemeris alone —
// is independent of the solve, so it applies identically on every disposition (emit /
// σ̂0-withhold / solve-fail).  The IODE this epoch uses is read straight from the
// shared LiveEphemerisStore (Card 5's own equivalence note: toe granularity ≫
// intra-epoch light-travel, so the emit-site query lands on the version the emission
// path used), so no dependence on the post-solve stamp remains.
//
// Placed after the observability-gate `continue`: a gate-withheld epoch leaves the
// carried clock state untouched (its post-emit update never runs), so deferring the
// re-level to the next non-gate-withheld epoch keeps relevel_iode and the carried
// state in lock-step.  On the σ̂0/solve-fail paths the block DID run, the shift landed,
// and the (skipped) post-emit updates leave it in place — no double-shift either way.
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
        if (bit->second == new_iode) continue;               // no roll — the common case
        const int old_iode = bit->second;
        const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
        relevel_iode[sat] = new_iode;                        // baseline advances regardless
        if (shift) {
            // Re-base the carried clock state by −Δ_eph.  Offset / level term ONLY —
            // the drift (c1) and the 2×2 covariance are frame-invariant and untouched.
            const double d = *shift;
            if (auto oit = clock_osc.find(sat); oit != clock_osc.end() && oit->second.init)
                oit->second.x0 -= d;                          // two-state offset (clock_model)
            for (CarryPrior& cp : prev_carry)                 // D2 scalar clock carry (model off)
                if (cp.kind == SharedParamKind::Clock && cp.sat == sat) cp.value -= d;
            if (auto jit = prev_clk.find(sat); jit != prev_clk.end())
                jit->second.first -= d;                       // clock-monitor jump baseline
            // Card 1: the coupling stash references this clock via ŝ_prev terms —
            // re-level them into the new frame too, else every coupling row's RHS
            // is off by M·Δ_eph after a cutover (the row's functional u = r + Mᵀs
            // is frame-consistent only if ALL carried references move together).
            if (carry_cpl_cond) {
                for (auto& [lk, lcv] : local_carry)
                    for (CoupledShared& cs2 : lcv.coupling)
                        if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                            cs2.s_prev -= d;
                for (auto& [zk, zcv] : zwd_carry)
                    for (CoupledShared& cs2 : zcv.coupling)
                        if (cs2.kind == SharedParamKind::Clock && cs2.sat == sat)
                            cs2.s_prev -= d;
            }
            log->info("Network (Card 6): {} eph cutover IODE {}->{} — re-leveled carried "
                      "clock by {:.4f} m (−Δ_eph); no discontinuity bump.",
                      sat, old_iode, new_iode, -d);
            emitEvent("RELEVEL", "eph_cutover_relevel", sat, "", -d, std::nullopt,
                      "relevel", fmt::format("iode={}->{}", old_iode, new_iode));
        } else {
            // Prior version no longer retained → Δ_eph incomputable → the clock stepped
            // by an unknown amount.  Force a per-sat discontinuity (consumer resets this
            // sat's network-corrected ambiguity across the step); it lands at the sat's
            // next actual emit (below).  Effectively unreachable with per-epoch checking.
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
