/**
 * @file UnrecordedSatGate.cpp
 * @brief Decisions of the rule for satellites without their stream records (see the header).
 */

#include "realtime/UnrecordedSatGate.hpp"

#include <cmath>

namespace GnssProcessor {

std::size_t UnrecordedSatGate::ageBin(double age_s) {
    if (!(age_s >= 0.0)) return 0;          // no record (negative or NaN)
    if (age_s <= 60.0)   return 1;
    if (age_s <= 300.0)  return 2;
    if (age_s <= 1800.0) return 3;
    return 4;
}

std::size_t UnrecordedSatGate::gapBin(double gap_s) {
    if (!(gap_s >= 0.0)) return 3;          // unknown counts as longer
    if (gap_s <= 60.0)   return 0;
    if (gap_s <= 300.0)  return 1;
    if (gap_s <= 1800.0) return 2;
    return 3;
}

const char* UnrecordedSatGate::causeName(DropCause c) {
    switch (c) {
        case DropCause::FirstSighting: return "first sighting";
        case DropCause::RecordLost:    return "record lost";
        case DropCause::BackOnStale:   return "back on a stale record";
    }
    return "unknown";
}

std::vector<UnrecordedSatGate::Event> UnrecordedSatGate::step(const std::vector<SatState>& sats,
                                                              std::set<std::string>& out,
                                                              double t_s, std::size_t min_kept) {
    out.clear();
    std::vector<Event> events;
    std::set<std::string> decided;
    std::size_t kept = 0;
    ++counters_.epochs;
    for (const SatState& s : sats) {
        if (!decided.insert(s.sat).second) continue;   // listed twice: the first entry decides
        ++counters_.sat_epochs_seen;
        if (s.missing == Missing::None) {
            ++kept;
            ever_recorded_.insert(s.sat);
            if (dropped_.erase(s.sat) > 0) {
                ++counters_.readmits;
                Event ev;
                ev.kind = EventKind::Readmit;
                ev.sat  = s.sat;
                const auto it = drop_time_.find(s.sat);
                if (it != drop_time_.end()) {
                    if (std::isfinite(it->second) && std::isfinite(t_s)) ev.gap_s = t_s - it->second;
                    drop_time_.erase(it);
                }
                ++counters_.readmits_by_gap[gapBin(ev.gap_s)];
                events.push_back(ev);
            }
            continue;
        }
        out.insert(s.sat);
        ++counters_.sat_epochs_out;
        if (s.missing == Missing::PhaseBias) ++counters_.sat_epochs_out_no_phase_bias;
        if (dropped_.insert(s.sat).second) {
            drop_time_[s.sat] = t_s;
            Event ev;
            ev.kind           = EventKind::Drop;
            ev.sat            = s.sat;
            ev.first_sighting = (ever_recorded_.count(s.sat) == 0);
            ev.missing        = s.missing;
            ev.signal         = s.missing == Missing::PhaseBias ? s.signal : std::string{};
            ev.cause          = ev.first_sighting           ? DropCause::FirstSighting
                              : seen_prev_.count(s.sat) > 0 ? DropCause::RecordLost
                                                            : DropCause::BackOnStale;
            ev.record_age_s   = s.record_age_s;
            ev.was_reference  = s.was_reference;
            ++counters_.drops;
            if (ev.first_sighting) ++counters_.drops_first_sighting;
            if (ev.cause == DropCause::RecordLost)  ++counters_.drops_record_lost;
            if (ev.cause == DropCause::BackOnStale) ++counters_.drops_back_on_stale;
            if (s.missing == Missing::PhaseBias) ++counters_.drops_no_phase_bias;
            if (s.was_reference) ++counters_.references_lost;
            ++counters_.drops_by_age[ageBin(s.record_age_s)];
            events.push_back(ev);
        }
    }
    seen_prev_ = decided;

    // The solvable floor: below it the epoch has no corrected solution and every satellite is out.
    no_solution_ = (min_kept > 0 && kept < min_kept);
    if (no_solution_) {
        for (const std::string& sat : decided) out.insert(sat);
        ++counters_.epochs_no_solution;
        if (!no_solution_open_) {
            no_solution_open_ = true;
            ++counters_.no_solution_spells;
            Event ev;
            ev.kind = EventKind::NoSolutionBegin;
            ev.kept = kept;
            events.push_back(ev);
        }
    } else if (no_solution_open_) {
        no_solution_open_ = false;
        Event ev;
        ev.kind = EventKind::NoSolutionEnd;
        ev.kept = kept;
        events.push_back(ev);
    }
    return events;
}

std::vector<UnrecordedSatGate::Event> UnrecordedSatGate::step(
    const std::vector<std::pair<std::string, bool>>& sats, std::set<std::string>& out) {
    std::vector<SatState> states;
    states.reserve(sats.size());
    for (const auto& [sat, has_clock] : sats) {
        SatState s;
        s.sat     = sat;
        s.missing = has_clock ? Missing::None : Missing::Clock;
        states.push_back(s);
    }
    return step(states, out);
}

void UnrecordedSatGate::restore(const std::map<std::string, double>& drop_times,
                                const std::set<std::string>& ever_recorded) {
    dropped_.clear();
    drop_time_ = drop_times;
    for (const auto& kv : drop_times) dropped_.insert(kv.first);
    ever_recorded_ = ever_recorded;
    seen_prev_.clear();
    no_solution_      = false;
    no_solution_open_ = false;
}

} // namespace GnssProcessor
