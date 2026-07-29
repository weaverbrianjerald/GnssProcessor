#pragma once
/**
 * @file NetworkRelevel.hpp
 * @brief Card 6 (RTF-2 / plan D3): the ephemeris-cutover clock RE-LEVEL of the PPP-RTK
 *        network producer.  On a broadcast IODE roll (old -> new) the estimable per-sat
 *        clock Delta-dts jumps by -Delta_eph relative to the a-priori it is measured
 *        against; this deterministically re-bases the CARRIED clock state by -Delta_eph so
 *        the solve runs entirely in the new frame with no discontinuity bump (the consumer
 *        reconstructs brdc(new_iode)+C0, Card 7).  On an incomputable shift (prior eph
 *        version evicted) it forces a per-sat discontinuity instead.
 *
 * Relocated VERBATIM out of @ref networkSolveGlue at the Card-6 relevel decomposition
 * (2026-07-23) — the long-planned "separate emit-region block" the INC-9/10 cuts left in
 * place (see NetworkClockQc.hpp banner).  Owns NO state: every carry object is a reference
 * into the caller's NetworkRunContext.
 *
 * @warning ORDERING — MUST run AFTER the observability-gate `continue` (a gate-withheld
 *          epoch leaves the carried clock untouched, so the re-level defers with
 *          relevel_iode in lock-step) and BEFORE the D2 / clock-osc prior injection and
 *          the solve (shifting the PRIOR is unconditionally exact for any prior weight,
 *          unlike a post-solve x_s shift).  Mutates ctx.carry.{relevel_iode,
 *          eph_cutover_disc_pending, clock_osc, prev_carry, prev_clk, local_carry,
 *          zwd_carry}.  brdc_rts-only (relevel_iode is meaningful only there).
 */

#include "rinex/MultiRinexSync.hpp"   // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Apply the Card-6 ephemeris-cutover re-level for this epoch.
 * @param ctx                   [in/out] carry (relevel_iode / eph_cutover_disc_pending /
 *                              clock_osc / prev_carry / prev_clk / local_carry / zwd_carry)
 *                              and knobs (carry_cpl_cond).  Taken whole so a dropped `&`
 *                              cannot silently drop a carry mutation; the body opens with
 *                              an alias preamble that byte-copies the former SolveGlue block.
 * @param log                   "main_log": the per-sat re-level / forced-disc lines.
 * @param traces                the emitEvent forwarder (RELEVEL tier events).
 * @param se                    current synced epoch (LiveEphemerisStore query + event time).
 * @param registered_clock_sats this epoch's registered satellite-clock keys.
 * @param brdc_rts              gate: only the broadcast-anchored (brdc_rts) run carries the
 *                              relevel_iode baseline.  A no-op otherwise.
 */
void networkEphCutoverRelevel(NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkRunTraces& traces,
                              const SyncedEpoch& se,
                              const std::vector<std::string>& registered_clock_sats,
                              bool brdc_rts);

} // namespace GnssProcessor
