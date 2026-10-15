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
#include "common/GnssIdentity.hpp"   // letterOfSat
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
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
        if (letterOfSat(s.sat) != constellation) continue;
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
        if (ctx.knobs.emit_pb_hold_phase_clock || ctx.knobs.emit_pb_hold_smoothed)
            ctx.carry.pb_hold_frz_prev_clk.clear();
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
    // Card SMH (network.emit_pb_hold_smoothed) runs the same machinery: it is the
    // transfer a key reverts to when its clock clone dies, and the shadow the clone's
    // transfer is compared with.
    const bool smh_on = ctx.knobs.emit_pb_hold_smoothed;
    const bool frz_on = ctx.knobs.emit_pb_hold_phase_clock || smh_on;
    const long long loop_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
    if (smh_on && ctx.carry.smh_t_first_s < 0.0) ctx.carry.smh_t_first_s = epoch_s;
    std::map<std::string, double> frz_c;   // satellite -> c~(t), satellites with a clock
    if (frz_on) {
        std::set<char> ccs;
        for (const auto& s : sol.sats)
            if (s.has_clock && !s.sat.empty() && std::isfinite(s.clock_c0_m))
                ccs.insert(letterOfSat(s.sat));
        bool advanced = false;
        for (char cc : ccs) {
            double med = 0.0;
            if (pbHoldCommonClockStep(sol, ctx.carry.pb_hold_frz_prev_clk, cc, med)) {
                net::put(ctx.entities.constellations, systemFromLetter(cc),
                         &net::ConstellationState::pb_hold_frz_M) += med;
                advanced = true;
            }
        }
        if (advanced) ++ctx.counters.n_pbh_frz_epochs;
        ctx.carry.pb_hold_frz_prev_clk.clear();
        for (const auto& s : sol.sats) {
            if (!s.has_clock || s.sat.empty() || !std::isfinite(s.clock_c0_m)) continue;
            ctx.carry.pb_hold_frz_prev_clk[s.sat] = s.clock_c0_m;
            frz_c[s.sat] = s.clock_c0_m - net::put(ctx.entities.constellations, systemOfSat(s.sat),
                                                   &net::ConstellationState::pb_hold_frz_M);
        }
    }

    for (auto& s : sol.sats) {
        if (s.sat.empty() || s.phase_bias.empty()) continue;
        const auto     fcit    = frz_on ? frz_c.find(s.sat) : frz_c.end();
        const bool     has_frz = frz_on && fcit != frz_c.end();
        const unsigned marked  = emitter.pendingMarkChannels(s.sat);
        const bool     coupled = (marked & NetworkProductEmitter::kDiscChanClock)
                                 && (marked & NetworkProductEmitter::kDiscChanPhaseBias);
        bool counted_coupled = false;

        for (auto& [signal, npb] : s.phase_bias) {
            const std::string key = s.sat + "|" + signal;
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
                if (has_frz) {                // the phase-visible clock is held from here (card LAH)
                    st.frz_ref = fcit->second;
                    st.frz_ok  = true;
                }
                if (smh_on) {
                    // Card SMH: the generation's clock clone is born at this epoch's
                    // stash, or at the first later one at which card Q3's anchor holds
                    // the clock (section 9); the previous generation's clone retires here.
                    ctx.carry.smh_clone.erase(key);
                    PbHoldClone pc;
                    pc.sat        = s.sat;
                    pc.signal     = signal;
                    pc.generation = st.generation;
                    pc.queued_ep  = loop_ep;
                    ctx.carry.smh_pending[key] = pc;
                    // Card SMD: the generation's block clone, under the same rule.
                    ctx.carry.smh_block.erase(key);
                    pc.block = true;
                    ctx.carry.smh_block_pending[key] = pc;
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
            double held_emit = st.held_m;
            // Card LAH: the held phase bias moves with the satellite's common-removed
            // clock, so c - p stays at its generation-start value.  An epoch without the
            // satellite's clock reuses the last transfer; a generation started without
            // one is held plainly (both counted).
            // Card SMH: a live clone updated this epoch writes LAH's transfer minus the
            // clone's frame-free passed motion Phi (section 10): the members of the hold
            // family share LAH's frame and median and differ only by Phi, so a smoothed key
            // and a bridged neighbour stay consistent.  A key with no live clone of its
            // generation (not yet born, or killed by the anchor's disengagement) bridges on
            // LAH's transfer until a clone is born (section 9).  A clone dead of any other
            // cause reverts the key to LAH's transfer for the rest of the generation.
            // Every switch is continuous.
            bool smh_written = false;
            if (smh_on) {
                // Card SMD: the key's block clone against the emitter's marks on the
                // satellite's phase channel (a retirement, or its unmarked steps and the
                // split of its passed motion).
                networkPbHoldBlockAtHold(
                    ctx, key, st.generation,
                    (marked & NetworkProductEmitter::kDiscChanPhaseBias) != 0u, epoch_s,
                    [&emitter, &s](const std::string& sg) { return emitter.discThresholdM(s.sat, sg); });
                // The member that writes: card SMH's clock clone.  Card SMD's block clone
                // is an instrument only (above, and the trace columns below).
                auto&      wmap = ctx.carry.smh_clone;
                const auto cit  = wmap.find(key);
                const bool mine = cit != wmap.end() && cit->second.generation == st.generation;
                const bool live = !st.smh_reverted && mine && cit->second.alive
                                  && cit->second.updated_ep == loop_ep;
                const bool structural = mine && !cit->second.alive
                                        && cit->second.death != PbHoldCloneDeath::Withdrawn
                                        && cit->second.death != PbHoldCloneDeath::Disengaged
                                        && cit->second.death != PbHoldCloneDeath::Followed
                                        && cit->second.death != PbHoldCloneDeath::PairLost;
                if (live) {
                    const PbHoldClone& c = cit->second;
                    const double nan = std::numeric_limits<double>::quiet_NaN();
                    // The writing row: the clock row.
                    const double c_phi     = c.phi;
                    const bool   c_phi_ok  = c.phi_ok;
                    const double c_W       = c.W;
                    const bool   c_w_valid = c.w_valid;
                    const double c_vpost   = c.vpost;
                    const double c_var     = c.var;
                    if (!st.smh_on_clone) {
                        st.smh_on_clone = true;
                        // LAH's reference continues from the transfer the key reached: a
                        // generation started without an emitted clock (no reference yet), or
                        // a bridge still waiting for one, takes it here.
                        if ((!st.frz_ok || st.smh_ref_pending) && has_frz) {
                            st.frz_ref         = fcit->second - st.frz_last;
                            st.frz_ok          = true;
                            st.smh_ref_pending = false;
                        }
                        st.smh_lah_last = st.frz_last;
                    }
                    // LAH's transfer, the frame every member shares; an epoch without the
                    // satellite's clock reuses the last one.
                    const double lah_m = (st.frz_ok && has_frz) ? fcit->second - st.frz_ref : nan;
                    if (std::isfinite(lah_m)) st.smh_lah_last = lah_m;
                    const double xfer_m = st.smh_lah_last - c_phi;
                    st.smh_last  = xfer_m;
                    held_emit   += xfer_m;
                    smh_written  = true;
                    ++ctx.counters.smh_held_smoothed;
                    ctx.counters.smh_xfer_abs.push_back(std::abs(xfer_m));
                    if (std::isfinite(lah_m)) ctx.counters.smh_diff_abs.push_back(std::abs(xfer_m - lah_m));
                    double r = nan;
                    if (c_phi_ok) {
                        ctx.counters.smh_phi_abs.push_back(std::abs(c_phi));
                        const double den = c_W - c_vpost;
                        if (!c_w_valid) {
                            ++ctx.counters.smh_w_invalid;
                        } else if (den > 0.0) {
                            r = c_phi * c_phi / den;
                            const int hour = static_cast<int>(std::floor((epoch_s - ctx.carry.smh_t_first_s) / 3600.0));
                            auto& acc = ctx.counters.smh_r[{letterOfSat(s.sat), hour}];
                            acc.first  += r;
                            acc.second += 1;
                        }
                    }
                    if (traces.pb_hold_clone_trace.is_open()) {
                        // Card SMD: the key's block clone beside the writing clock clone (the
                        // split of its passed motion on its own birth), empty when not live.
                        // Card REV: then the revision, the step, the stated variance of D(t0)
                        // (now and at birth), the filtered variance of D and the epoch's walk.
                        std::string bcols = ",,,,,,,,,,,,,";
                        const auto bit = ctx.carry.smh_block.find(key);
                        if (bit != ctx.carry.smh_block.end() && bit->second.generation == st.generation
                            && bit->second.alive && bit->second.updated_ep == loop_ep
                            && bit->second.phi_ok && bit->second.pv_phi_ok) {
                            const PbHoldClone& b = bit->second;
                            const double bden = b.pv_W - b.pv_vpost;
                            std::string pair;
                            for (std::size_t j = 0; j < b.pv_pair.sig.size(); ++j)
                                pair += (j ? "+" : "") + b.pv_pair.sig[j];
                            bcols = fmt::format("{:.1f},{:.6f},{:.6f},{:.6f},{:.6e},{:.6e},", epoch_s - b.t0_s,
                                                b.phi, b.pv_phi, b.phi - b.pv_phi, b.pv_W, b.pv_vpost)
                                    + (b.pv_w_valid && bden > 0.0
                                           ? fmt::format("{:.4f}", b.pv_phi * b.pv_phi / bden) : std::string{})
                                    + ',' + pair
                                    + fmt::format(",{:.6f},{:.6f},{:.9e},{:.9e},{:.9e},{:.9e}", b.pv_rev,
                                                  b.pv_sd, b.pv_vd0, b.pv_vd0_birth, b.pv_pd, b.pv_dw);
                        }
                        traces.pb_hold_clone_trace
                            << sol.epoch_time.week << ',' << fmt::format("{:.3f}", sol.epoch_time.tow) << ','
                            << s.sat << ',' << signal << ',' << st.generation << ','
                            << fmt::format("{:.1f},{:.6f},", epoch_s - c.t0_s, xfer_m)
                            << (std::isfinite(lah_m) ? fmt::format("{:.6f}", lah_m) : std::string{}) << ','
                            << (c_phi_ok ? fmt::format("{:.6f}", c_phi) : std::string{}) << ','
                            << fmt::format("{:.6e},{:.6e},{:.6e},", c_W, c_vpost, c_var)
                            << (std::isfinite(r) ? fmt::format("{:.4f}", r) : std::string{}) << ','
                            << (std::isfinite(c.pd0) ? fmt::format("{:.6e}", c.pd0) : std::string{}) << ','
                            << (std::isfinite(c.f0) ? fmt::format("{:.4f}", c.f0) : std::string{}) << ','
                            << "clk" << ',' << bcols << '\n';
                    }
                } else if (!st.smh_reverted) {
                    // Leaving the clone: LAH's transfer continues with the passed motion
                    // the clone had reached folded into LAH's reference, so this epoch's
                    // clock step is transferred as LAH transfers it and C - P stays
                    // continuous (card SMD section 7: re-referencing LAH on the last
                    // written transfer dropped that step, a step of C - P at every switch).
                    // Without an LAH reference yet, it is taken at the next emitted clock.
                    // A key already on the bridge is continuous as it stands.
                    const bool was_on = st.smh_on_clone;
                    if (was_on) {
                        if (st.frz_ok) {
                            st.frz_ref += st.smh_lah_last - st.smh_last;
                        } else {
                            st.frz_ok          = true;
                            st.smh_ref_pending = true;
                        }
                        st.frz_last     = st.smh_last;
                        st.smh_on_clone = false;
                    }
                    const int why = mine ? static_cast<int>(cit->second.death) : -1;
                    if (structural) {
                        st.smh_reverted = true;
                        ++ctx.counters.smh_reverts;
                        traces.emitEvent(se.time, "PB_HOLD", "smh_revert", s.sat, "",
                                         st.frz_last, std::nullopt, "clone_dead",
                                         fmt::format("sig={} gen={} death={} transfer={:.6f} m",
                                                     signal, st.generation, why, st.frz_last));
                    } else if (was_on) {
                        ++ctx.counters.smh_bridges;
                        traces.emitEvent(se.time, "PB_HOLD", "smh_bridge", s.sat, "",
                                         st.frz_last, std::nullopt,
                                         why == static_cast<int>(PbHoldCloneDeath::Withdrawn) ? "withdrawn"
                                         : why == static_cast<int>(PbHoldCloneDeath::Followed) ? "followed"
                                         : why == static_cast<int>(PbHoldCloneDeath::PairLost) ? "pair_lost"
                                                                                                : "disengaged",
                                         fmt::format("sig={} gen={} death={} transfer={:.6f} m",
                                                     signal, st.generation, why, st.frz_last));
                    }
                }
            }
            if (frz_on && !smh_written) {
                if (smh_on) {
                    if (st.smh_reverted) ++ctx.counters.smh_held_lah;
                    else                 ++ctx.counters.smh_held_bridged;
                }
                if (st.smh_ref_pending && has_frz) {
                    // Continue LAH's transfer from the clone's last one (card SMH).
                    st.frz_ref         = fcit->second - st.frz_last;
                    st.smh_ref_pending = false;
                }
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

            if (coupled && !counted_coupled && frz_on && !smh_written && st.frz_ok && has_frz) {
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
