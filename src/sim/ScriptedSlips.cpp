/**
 * @file ScriptedSlips.cpp
 * @brief Implementation of the deterministic scripted cycle-slip schedule.
 *
 * Pure logic: no RNG, no I/O, no product dependencies (see ScriptedSlips.hpp).
 */

#include "sim/ScriptedSlips.hpp"

#include <cstddef>
#include <sstream>

namespace GnssProcessor {
namespace sim {

ScriptedSlipSchedule::ScriptedSlipSchedule(const std::vector<ScriptedSlip>& events) {
    events_.reserve(events.size());
    for (const auto& e : events)
        events_.push_back(EventState{ e, /*started=*/false, /*applied=*/0 });
}

std::vector<ScriptedSlipSchedule::Fired>
ScriptedSlipSchedule::onEpoch(const std::string& sat, const std::string& phase_code,
                              int epoch_counter, double elev_deg) {
    std::vector<Fired> out;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        EventState& st = events_[i];
        const ScriptedSlip& e = st.ev;
        if (e.sat != sat || e.signal != phase_code) continue; // not this (sat, signal)
        if (st.applied >= e.repeat) continue;                 // all repetitions done

        if (!st.started) {
            bool trigger = false;
            if (e.epoch > 0) {
                // Form A: fire only at the exact epoch (satellite must be visible then).
                trigger = (epoch_counter == e.epoch);
            } else {
                // Form B: first visible epoch at/after not_before inside the window.
                trigger = (epoch_counter >= e.not_before_epoch &&
                           elev_deg >= e.elev_lo_deg && elev_deg < e.elev_hi_deg);
            }
            if (!trigger) continue;
            st.started = true;
        }

        // Started (this call or an earlier one): apply one repetition on this
        // visible epoch.  Reports lli = flag_lli for every repetition.
        out.push_back(Fired{ e.delta_cycles, e.flag_lli, static_cast<int>(i) });
        ++st.applied;
    }
    return out;
}

std::vector<std::string> ScriptedSlipSchedule::unfinished() const {
    std::vector<std::string> problems;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const EventState& st = events_[i];
        const ScriptedSlip& e = st.ev;

        std::ostringstream trig;
        if (e.epoch > 0) {
            trig << "epoch " << e.epoch;
        } else {
            trig << "elev [" << e.elev_lo_deg << ", " << e.elev_hi_deg
                 << ") not_before " << e.not_before_epoch;
        }

        std::ostringstream msg;
        if (!st.started) {
            msg << "scripted slip #" << i << " (" << e.sat << " " << e.signal << ", " << trig.str()
                << ") never fired";
            problems.push_back(msg.str());
        } else if (st.applied < e.repeat) {
            msg << "scripted slip #" << i << " (" << e.sat << " " << e.signal << ", " << trig.str()
                << ") applied only " << st.applied << "/" << e.repeat
                << " repetitions (arc ended mid-repeat)";
            problems.push_back(msg.str());
        }
    }
    return problems;
}

} // namespace sim
} // namespace GnssProcessor
