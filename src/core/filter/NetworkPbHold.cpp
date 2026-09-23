/**
 * @file NetworkPbHold.cpp
 * @brief Implementation of @ref networkPbHold — card PBH's emit-side phase-bias
 *        hold (@c network.emit_pb_hold).
 *
 * New code, not a relocation.  Seam contract: a free function over
 * @c (NetworkRunContext&, const GnssConfig&, log, NetworkRunTraces&, …) that owns
 * NO state — every persistent object is a reference into @c ctx.carry.
 */

#include "filter/NetworkPbHold.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// The epoch's de-commoned clock step per satellite, in metres.
///
/// `d_s = (c_s(k) - c_s(k-1)) - median_s(c_s(k) - c_s(k-1))`.  The median over the
/// satellites present at BOTH epochs removes the per-constellation common mode a
/// clock datum re-realization injects, which is exactly the part a consumer's own
/// receiver clock absorbs — so what is left is the satellite-specific step, the
/// quantity a coupled frame move would have to transfer.  Satellites without a
/// previous emission are absent from the result (no step is defined for them).
std::map<std::string, double> decommonedClockStep(
    const NetworkSolutionEpoch& sol,
    const std::map<std::string, double>& prev_clock_m) {
    std::map<std::string, double> raw;
    std::vector<double>           pop;
    for (const auto& s : sol.sats) {
        if (!s.has_clock || s.sat.empty()) continue;
        const auto it = prev_clock_m.find(s.sat);
        if (it == prev_clock_m.end()) continue;
        const double d = s.clock_c0_m - it->second;
        if (!std::isfinite(d)) continue;
        raw[s.sat] = d;
        pop.push_back(d);
    }
    if (pop.empty()) return raw;
    std::sort(pop.begin(), pop.end());
    const std::size_t n = pop.size();
    const double med = (n % 2 == 1) ? pop[n / 2] : 0.5 * (pop[n / 2 - 1] + pop[n / 2]);
    for (auto& [sat, d] : raw) { (void)sat; d -= med; }
    return raw;
}

}  // namespace

void networkPbHold(NetworkRunContext& ctx,
                   const GnssConfig& config_,
                   const std::shared_ptr<spdlog::logger>& log,
                   NetworkRunTraces& traces,
                   const NetworkProductEmitter& emitter,
                   const SyncedEpoch& se,
                   NetworkSolutionEpoch& sol) {
    (void)config_;   // the live values are the ctx.knobs snapshot (seam contract)
    if (!ctx.knobs.emit_pb_hold || sol.sats.empty()) return;

    auto&        hold      = ctx.carry.pb_hold;
    auto&        prev_clk  = ctx.carry.pb_hold_prev_clock_m;
    const double max_gap_s = ctx.knobs.emit_pb_hold_max_gap_s;
    const double epoch_s   = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;

    // Invariant #7, the COUPLED edge.  A mark naming BOTH the clock and the phase
    // bias of a satellite asserts that the two moved together — the one case where
    // an identity-passing contract is knowingly lossy, because the hold refuses the
    // phase-bias half of the move.  The refusal is not hidden: the satellite's
    // de-commoned clock step at that epoch is recorded, so the run-end summary
    // states the SIZE of what was not transferred.  Computed from the marks PENDING
    // at this point, which is the complete set: this seam is the last thing before
    // emitToStore, and emitToStore is what consumes them.
    const std::map<std::string, double> clk_step = decommonedClockStep(sol, prev_clk);

    bool logged_arm = ctx.counters.n_pbh_held_emissions > 0
                      || ctx.counters.n_pbh_gen_first > 0;

    for (auto& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        // Card CLV (network.clock_level_carry): the satellite's accumulated level shift
        // per signal.  The re-base moves the carried clock AND every phase bias of the
        // satellite by the same amount, so the estimator's phase side does not move; a
        // hold that froze P while C moved would move the wire's phase side instead.
        // Null on an unarmed run (never read).
        const std::map<std::string, double>* xfer = nullptr;
        if (ctx.knobs.clv_on) {
            const auto lit = ctx.carry.clk_level.sat.find(s.sat);
            if (lit != ctx.carry.clk_level.sat.end()) xfer = &lit->second.xfer;
        }
        const unsigned marked  = emitter.pendingMarkChannels(s.sat);
        const bool     coupled = (marked & NetworkProductEmitter::kDiscChanClock)
                                 && (marked & NetworkProductEmitter::kDiscChanPhaseBias);
        bool counted_coupled = false;

        for (auto& [signal, npb] : s.phase_bias) {
            const std::string key = s.sat + "|" + signal;
            double level_acc = 0.0;   // card CLV: this signal's accumulated level shift
            if (xfer) {
                const auto xit = xfer->find(signal);
                if (xit != xfer->end()) level_acc = xit->second;
            }
            auto  it    = hold.find(key);
            const bool first = (it == hold.end());
            const bool gap   = !first && max_gap_s > 0.0
                               && (epoch_s - it->second.last_emit_s) > max_gap_s;

            if (first || gap) {
                // RE-COUPLING edge: a generation STARTS here.  The current estimate
                // becomes the held value — the one epoch per generation at which the
                // wire tracks the estimator — and the declaration rides the stamped
                // generation, which the consumer reacts to.
                NetworkCarryState::PbHoldKey st;
                st.generation  = first ? 1u : (it->second.generation + 1u);
                st.held_m      = npb.bias_m;
                st.last_emit_s = epoch_s;
                st.level_ref   = level_acc;   // card CLV: the transfer counts from here
                if (first) {
                    hold.emplace(key, st);
                    ++ctx.counters.n_pbh_keys;
                    ++ctx.counters.n_pbh_gen_first;
                } else {
                    const double prev_gap = epoch_s - it->second.last_emit_s;
                    it->second = st;
                    ++ctx.counters.n_pbh_gen_gap;
                    traces.emitEvent(se.time, "PB_HOLD", "gen_start", s.sat, "",
                                     prev_gap, max_gap_s, "gap",
                                     fmt::format("sig={} gen={} held={:.6f} m gap={:.1f} s",
                                                 signal, st.generation, st.held_m, prev_gap));
                }
                if (first)
                    traces.emitEvent(se.time, "PB_HOLD", "gen_start", s.sat, "",
                                     std::nullopt, std::nullopt, "first",
                                     fmt::format("sig={} gen={} held={:.6f} m",
                                                 signal, st.generation, st.held_m));
                npb.hold_generation = st.generation;
                // The emitted value is the estimate on the generation's first epoch,
                // so there is no deviation to record and nothing was hidden.
                continue;
            }

            // DECOUPLING edge: the hold writes.  |estimate - held| is the whole cost,
            // and it is measured on EVERY held emission rather than sampled.
            auto& st = it->second;
            // Card CLV: the held value follows the level shift (the satellite's level
            // step is declared on its C record, see networkClockLevelDeclare, so the
            // emitter's value-jump test does not read the level as a discontinuity).
            // OFF: the arithmetic is the pre-card one.
            double held_emit = st.held_m;
            if (ctx.knobs.clv_on) {
                const double level_off = level_acc - st.level_ref;
                held_emit = st.held_m + level_off;
                if (level_off != 0.0) ++ctx.counters.clv_held_moved;
            }
            const double dev = npb.bias_m - held_emit;
            if (std::isfinite(dev)) ctx.carry.pb_hold_dev_abs_m.push_back(std::abs(dev));
            npb.bias_m          = held_emit;
            npb.hold_generation = st.generation;
            st.last_emit_s      = epoch_s;
            ++ctx.counters.n_pbh_held_emissions;

            if (coupled && !counted_coupled) {
                counted_coupled = true;
                ++ctx.counters.n_pbh_coupled_marks;
                const auto cit = clk_step.find(s.sat);
                const double step = (cit == clk_step.end()) ? 0.0 : cit->second;
                ctx.carry.pb_hold_coupled_step_m.push_back(std::abs(step));
                traces.emitEvent(se.time, "PB_HOLD", "coupled_mark", s.sat, "",
                                 step, std::nullopt, "not_transferred",
                                 fmt::format("sig={} gen={} de-commoned clock step the hold "
                                             "does NOT transfer to the phase bias",
                                             signal, st.generation));
            }
        }
    }

    // Refresh the clock baseline for the next epoch's de-commoned step.  Written
    // AFTER the sweep so the step above is against the previous EMISSION, and only
    // for satellites this epoch actually carries a clock for.
    for (const auto& s : sol.sats)
        if (s.has_clock && !s.sat.empty()) prev_clk[s.sat] = s.clock_c0_m;

    if (!logged_arm && log && ctx.counters.n_pbh_gen_first > 0) {
        logged_arm = true;
        log->info("Network (card PBH): emit-side phase-bias HOLD armed — {} key(s) held at "
                  "their first emitted value, max_gap_s {:.1f}.  The estimator keeps running "
                  "behind the hold; the solve, the C records and the B records are unchanged.",
                  ctx.counters.n_pbh_gen_first, max_gap_s);
    }
}

}  // namespace GnssProcessor
