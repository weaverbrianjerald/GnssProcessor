/**
 * @file NetworkPbHold.cpp
 * @brief Implementation of @ref networkPbHold, the emit-side phase-bias hold
 *        (@c network.emit_pb_hold, card PBH).
 *
 * Written as a seam, not relocated from the driver.  Seam contract: a free function
 * over @c (NetworkRunContext&, const GnssConfig&, log, NetworkRunTraces&, …) that
 * owns no state; every persistent object is a reference into @c ctx.carry.
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
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// The epoch's de-commoned clock step per satellite, in metres.
///
/// `d_s = (c_s(k) - c_s(k-1)) - median_s(c_s(k) - c_s(k-1))`.  The median is taken
/// over the satellites with a clock at both epochs, across all constellations at
/// once.  It removes the common mode a clock datum re-realization injects, which a
/// consumer's own receiver clock absorbs; the remainder is the satellite-specific
/// step, the quantity a coupled frame move would have to transfer.  Satellites
/// without a previous clock are absent from the result (no step is defined).
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

/// Card LAH: a held emission counts as moved by the transfer above this (m).
constexpr double kFrzMovedM = 1e-6;

}  // namespace

bool pbHoldCommonClockStep(const NetworkSolutionEpoch& sol,
                           const std::map<std::string, double>& prev_clock_m,
                           char constellation, double& median_m) {
    std::vector<double> pop;
    for (const auto& s : sol.sats) {
        if (!s.has_clock || s.sat.empty() || !std::isfinite(s.clock_c0_m)) continue;
        if (s.sat[0] != constellation) continue;
        const auto it = prev_clock_m.find(s.sat);
        if (it == prev_clock_m.end()) continue;
        pop.push_back(s.clock_c0_m - it->second);
    }
    if (pop.empty()) return false;
    std::sort(pop.begin(), pop.end());
    const std::size_t n = pop.size();
    median_m = (n % 2 == 1) ? pop[n / 2] : 0.5 * (pop[n / 2 - 1] + pop[n / 2]);
    return true;
}

void networkPbHold(NetworkRunContext& ctx,
                   const GnssConfig& config_,
                   const std::shared_ptr<spdlog::logger>& log,
                   NetworkRunTraces& traces,
                   const NetworkProductEmitter& emitter,
                   const SyncedEpoch& se,
                   NetworkSolutionEpoch& sol) {
    (void)config_;   // the live values are the ctx.knobs snapshot (seam contract)
    if (!ctx.knobs.emit_pb_hold) return;
    if (sol.sats.empty()) {
        // An emitted epoch with no satellite (every one pruned) is still an emission, so
        // the phase-clock mode's next step is taken against it: nothing (card LAH).
        if (ctx.knobs.emit_pb_hold_phase_clock) ctx.carry.pb_hold_frz_prev_clk.clear();
        return;
    }

    auto&        hold      = ctx.carry.pb_hold;
    auto&        prev_clk  = ctx.carry.pb_hold_prev_clock_m;
    const double max_gap_s = ctx.knobs.emit_pb_hold_max_gap_s;
    const double epoch_s   = sol.epoch_time.week * 604800.0 + sol.epoch_time.tow;

    // Coupled marks (invariant #7).  A mark naming both the clock and the phase bias
    // of a satellite states that the two moved together.  This is the one case in
    // which the identity-passing contract loses information, because the hold does
    // not pass on the phase-bias half of the move.  The satellite's de-commoned clock
    // step at that epoch is recorded, so the run-end summary states the size of what
    // was not transferred.  The marks pending at this point are the complete set:
    // every marking site has run, and emitToStore, which consumes them, follows.
    const std::map<std::string, double> clk_step = decommonedClockStep(sol, prev_clk);

    bool logged_arm = ctx.counters.n_pbh_held_emissions > 0
                      || ctx.counters.n_pbh_gen_first > 0;

    // Card LAH (network.emit_pb_hold_phase_clock): the held key keeps the satellite's
    // phase-visible clock c - p instead of its phase bias.  c~ = c - M_k, with M_k the
    // accumulated median emitted-clock step of constellation k over its satellites
    // present at this and the previous emission.  A move common to a constellation (its
    // pivot handover, the frame hold's per-constellation re-anchor) therefore stays on
    // the clock, where the consumer's receiver clock and inter-system bias absorb it on
    // code and phase alike, and a satellite's own move (its level included) is written
    // into its held phase biases as well.  Built before the sweep from the clocks about
    // to be emitted (the frame-held values in sol).
    const bool frz_on = ctx.knobs.emit_pb_hold_phase_clock;
    std::map<std::string, double> frz_c;   // satellite -> c~(t), satellites with a clock
    if (frz_on) {
        std::set<char> ccs;
        for (const auto& s : sol.sats)
            if (s.has_clock && !s.sat.empty() && std::isfinite(s.clock_c0_m)) ccs.insert(s.sat[0]);
        bool advanced = false;
        for (char cc : ccs) {
            double med = 0.0;
            if (pbHoldCommonClockStep(sol, ctx.carry.pb_hold_frz_prev_clk, cc, med)) {
                ctx.carry.pb_hold_frz_M[cc] += med;
                advanced = true;
            }
        }
        if (advanced) ++ctx.counters.n_pbh_frz_epochs;
        ctx.carry.pb_hold_frz_prev_clk.clear();
        for (const auto& s : sol.sats) {
            if (!s.has_clock || s.sat.empty() || !std::isfinite(s.clock_c0_m)) continue;
            ctx.carry.pb_hold_frz_prev_clk[s.sat] = s.clock_c0_m;
            frz_c[s.sat] = s.clock_c0_m - ctx.carry.pb_hold_frz_M[s.sat[0]];
        }
    }

    for (auto& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        // The satellite's accumulated clock-level shift per signal
        // (network.clock_level_carry, card CLV).  The re-base moves the carried clock
        // and every phase bias of the satellite by the same amount, so the estimator's
        // phase side does not move; a hold that froze P while C moved would move the
        // wire's phase side instead.  Null when CLV is not armed (then never read).
        const std::map<std::string, double>* xfer = nullptr;
        if (ctx.knobs.clv_on) {
            const auto lit = ctx.carry.clk_level.sat.find(s.sat);
            if (lit != ctx.carry.clk_level.sat.end()) xfer = &lit->second.xfer;
        }
        const auto     fcit    = frz_on ? frz_c.find(s.sat) : frz_c.end();
        const bool     has_frz = frz_on && fcit != frz_c.end();
        const unsigned marked  = emitter.pendingMarkChannels(s.sat);
        const bool     coupled = (marked & NetworkProductEmitter::kDiscChanClock)
                                 && (marked & NetworkProductEmitter::kDiscChanPhaseBias);
        bool counted_coupled = false;

        for (auto& [signal, npb] : s.phase_bias) {
            const std::string key = s.sat + "|" + signal;
            double level_acc = 0.0;   // this signal's accumulated level shift (card CLV)
            if (xfer) {
                const auto xit = xfer->find(signal);
                if (xit != xfer->end()) level_acc = xit->second;
            }
            auto  it    = hold.find(key);
            const bool first = (it == hold.end());
            const bool gap   = !first && max_gap_s > 0.0
                               && (epoch_s - it->second.last_emit_s) > max_gap_s;

            if (first || gap) {
                // Re-coupling edge: a generation starts here.  The current estimate
                // becomes the held value (the one epoch per generation at which the
                // wire equals the estimate), and the declaration is the stamped
                // generation number, which the consumer reacts to.
                NetworkCarryState::PbHoldKey st;
                st.generation  = first ? 1u : (it->second.generation + 1u);
                st.held_m      = npb.bias_m;
                st.last_emit_s = epoch_s;
                st.level_ref   = level_acc;   // the level transfer counts from here (card CLV)
                if (has_frz) {                // the phase-visible clock is held from here (card LAH)
                    st.frz_ref = fcit->second;
                    st.frz_ok  = true;
                }
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

            // Decoupling edge: the hold writes.  |estimate - held| is the whole cost,
            // and it is recorded on every held emission, not sampled.
            auto& st = it->second;
            // With card CLV armed, the held value follows the level shift (the
            // satellite's level step is declared on its C record, see
            // networkClockLevelDeclare, so the emitter's value-jump test does not read
            // the level as a discontinuity).  With it off, the held value is emitted
            // unchanged.
            double held_emit = st.held_m;
            if (ctx.knobs.clv_on) {
                const double level_off = level_acc - st.level_ref;
                held_emit = st.held_m + level_off;
                if (level_off != 0.0) ++ctx.counters.clv_held_moved;
            }
            // Card LAH: the held phase bias moves with the satellite's common-removed
            // clock, so c - p stays at its generation-start value.  An epoch without the
            // satellite's clock reuses the last transfer; a generation started without
            // one is held plainly (both counted).
            if (frz_on) {
                if (!st.frz_ok) {
                    ++ctx.counters.n_pbh_frz_noref;
                } else {
                    double xfer_m = st.frz_last;
                    if (has_frz) {
                        xfer_m = fcit->second - st.frz_ref;
                        ctx.carry.pb_hold_frz_step_abs_m.push_back(std::abs(xfer_m - st.frz_last));
                    } else {
                        ++ctx.counters.n_pbh_frz_noclock;
                    }
                    st.frz_last = xfer_m;
                    held_emit += xfer_m;
                    // "Moved": the transfer is above a micrometre (an exact-zero test
                    // would count rounding residue of the median subtraction).
                    if (std::abs(xfer_m) > kFrzMovedM) ++ctx.counters.n_pbh_frz_moved;
                    ctx.carry.pb_hold_frz_total_abs_m.push_back(std::abs(xfer_m));
                }
            }
            const double dev = npb.bias_m - held_emit;
            if (std::isfinite(dev)) ctx.carry.pb_hold_dev_abs_m.push_back(std::abs(dev));
            npb.bias_m          = held_emit;
            npb.hold_generation = st.generation;
            st.last_emit_s      = epoch_s;
            ++ctx.counters.n_pbh_held_emissions;

            if (coupled && !counted_coupled && frz_on && st.frz_ok && has_frz) {
                // Card LAH: the mode writes the satellite's clock step into the held
                // phase bias, so a coupled move is transferred, not lost.
                counted_coupled = true;
                ++ctx.counters.n_pbh_frz_coupled;
            }
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

    // Refresh the clock baseline for the next epoch's de-commoned step.  Written after
    // the sweep, so the step above is taken against the previous emission, and only
    // for satellites that carry a clock this epoch.
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
