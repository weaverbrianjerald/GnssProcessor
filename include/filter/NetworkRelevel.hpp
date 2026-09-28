#pragma once
/**
 * @file NetworkRelevel.hpp
 * @brief Ephemeris-cutover clock re-level of the PPP-RTK network producer (card 6,
 *        RTF-2 / plan D3).  When the broadcast IODE rolls (old -> new), the estimable
 *        per-satellite clock Delta-dts jumps by -Delta_eph relative to the a-priori it is
 *        measured against.  This seam re-bases the carried clock state by -Delta_eph, so
 *        the solve runs in the new frame with no discontinuity bump (the consumer
 *        reconstructs brdc(new_iode)+C0, card 7).  When the shift cannot be computed
 *        (the prior ephemeris version is no longer held), it forces a per-satellite
 *        discontinuity instead.
 *
 * Moved unchanged out of @ref networkSolveGlue as the separate emit-region block the
 * INC-9/10 cuts left in place (see the NetworkClockQc.hpp banner).  Owns no state: every
 * carry object is a reference into the caller's NetworkRunContext.
 *
 * @warning Ordering: call after the observability gate (a gate-withheld epoch leaves the
 *          carried clock untouched, so the re-level is deferred with relevel_iode in
 *          lock-step) and before the D2 / clock-osc prior injection and the solve.
 *          Shifting the prior is exact for any prior weight; a post-solve x_s shift is
 *          not.  Mutates ctx.carry.{relevel_iode, eph_cutover_disc_pending, clock_osc,
 *          prev_carry, prev_clk, local_carry, zwd_carry}.  Acts only when brdc_rts is
 *          true, the only mode in which relevel_iode is meaningful.
 */

#include "rinex/MultiRinexSync.hpp"   // MultiRinexSync::SyncedEpoch (bare SyncedEpoch)

#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Apply the ephemeris-cutover re-level for this epoch (card 6).
 * @param ctx                   [in/out] carry (relevel_iode / eph_cutover_disc_pending /
 *                              clock_osc / prev_carry / prev_clk / local_carry / zwd_carry)
 *                              and knobs (carry_cpl_cond).  Passed whole so that a missing
 *                              `&` cannot drop a carry mutation; the body opens with an
 *                              alias preamble copied from the former SolveGlue block.
 * @param log                   "main_log": the per-satellite re-level and forced-disc lines.
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
