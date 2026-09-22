/**
 * @file ArcLedger.cpp
 * @brief Card SID phase 1: the arc lifecycle state machine (see ArcLedger.hpp).
 */
#include "filter/ArcLedger.hpp"

#include <algorithm>

namespace GnssProcessor {

    const char* arcStateName(ArcState s) {
        switch (s) {
        case ArcState::Absent:    return "absent";
        case ArcState::Tracking:  return "tracking";
        case ArcState::Gap:       return "gap";
        case ArcState::Reentered: return "reentered";
        case ArcState::Dead:      return "dead";
        }
        return "?";
    }

    const char* arcTransitionName(ArcTransition t) {
        switch (t) {
        case ArcTransition::None:    return "none";
        case ArcTransition::Birth:   return "arc_birth";
        case ArcTransition::Gap:     return "arc_gap";
        case ArcTransition::Reentry: return "arc_reentry";
        case ArcTransition::Death:   return "arc_death";
        }
        return "?";
    }

    void ArcLedger::configure(double horizon_s, long long forget_ep) {
        horizon_s_ = horizon_s;
        forget_ep_ = std::max<long long>(0, forget_ep);
    }

    void ArcLedger::beginEpoch(long long epoch_idx, double t_s) {
        epoch_idx_ = epoch_idx;
        t_s_       = t_s;
        seen_.clear();
    }

    const ArcRecord* ArcLedger::find(const Key& key) const {
        const auto it = arcs_.find(key);
        return it == arcs_.end() ? nullptr : &it->second;
    }

    std::size_t ArcLedger::liveCount() const {
        std::size_t n = 0;
        for (const auto& [k, r] : arcs_) {
            (void)k;
            if (r.state == ArcState::Tracking || r.state == ArcState::Reentered) ++n;
        }
        return n;
    }

    std::size_t ArcLedger::admittedLiveCount() const {
        std::size_t n = 0;
        for (const auto& [k, r] : arcs_) {
            (void)k;
            if (r.adm_state == ArcState::Tracking || r.adm_state == ArcState::Reentered) ++n;
        }
        return n;
    }

    ArcStep ArcLedger::observe(const Key& key, bool modelled) {
        seen_.insert(key);
        auto it = arcs_.find(key);
        if (it == arcs_.end()) it = arcs_.emplace(key, ArcRecord{}).first;
        ArcRecord& a = it->second;

        ArcStep s;
        s.from = a.state;
        switch (a.state) {
        case ArcState::Absent:
        case ArcState::Dead:
            // A BIRTH.  The previous generation travels out on the step so a caller
            // can tell a true first sighting (0) from a return after a death, which
            // is the distinction the anchor election does not make today.
            s.transition     = ArcTransition::Birth;
            s.prev_generation = a.generation;
            s.gap_len_ep     = (a.ep_last_seen < 0)
                                 ? 0
                                 : static_cast<int>(epoch_idx_ - a.ep_last_seen - 1);
            ++a.generation;
            if (a.ep_first_seen < 0) a.ep_first_seen = epoch_idx_;
            a.state          = ArcState::Tracking;
            a.ep_state_changed = epoch_idx_;
            a.gap_len_ep     = 0;
            a.reason_mask    = kArcReasonNone;
            break;
        case ArcState::Gap:
            // A RE-ENTRY inside the horizon.  The gap length is the epochs missed,
            // which is the unit every existing re-entry instrument reports.
            s.transition = ArcTransition::Reentry;
            s.gap_len_ep = static_cast<int>(epoch_idx_ - a.ep_last_seen - 1);
            a.gap_len_ep = s.gap_len_ep;
            a.state      = ArcState::Reentered;
            a.ep_state_changed = epoch_idx_;
            break;
        case ArcState::Reentered:
            // The epoch after a re-entry: back to the steady state, silently.
            a.state      = ArcState::Tracking;
            a.ep_state_changed = epoch_idx_;
            a.gap_len_ep = 0;
            break;
        case ArcState::Tracking:
            break;
        }
        a.ep_last_seen  = epoch_idx_;
        a.t_last_seen_s = t_s_;
        a.modelled      = modelled;
        s.to            = a.state;
        s.generation    = a.generation;
        s.reason_mask   = a.reason_mask;
        // Card SID phase 2: the admitted side as it stands at this epoch, so a raw
        // re-entry event states BOTH gap lengths.  The admitted pass of this epoch
        // has not run yet (it is downstream), so the number is the admitted gap that
        // is OPEN as the raw arc returns, which is the quantity a reader of the raw
        // edge wants.
        s.adm_state      = a.adm_state;
        s.adm_gap_len_ep = a.admittedGapEp(epoch_idx_);
        s.adm_generation = a.adm_generation;
        return s;
    }

    void ArcLedger::endEpoch(const std::set<int>& present_stations,
                             std::vector<std::pair<Key, ArcStep>>& out) {
        out.clear();
        std::vector<Key> forget;
        for (auto& [key, a] : arcs_) {
            const int sta = std::get<0>(key);
            if (a.state == ArcState::Dead) {
                // The forget sweep runs for EVERY station, present or not: a record
                // that has been dead for the whole window carries no information a
                // station's return could revive.
                if (forget_ep_ > 0 && epoch_idx_ - a.ep_state_changed > forget_ep_)
                    forget.push_back(key);
                continue;
            }
            if (!present_stations.count(sta)) continue;   // station absent: frozen
            if (seen_.count(key)) continue;               // observed this epoch

            ArcStep s;
            s.from = a.state;
            if (a.state == ArcState::Tracking || a.state == ArcState::Reentered) {
                s.transition       = ArcTransition::Gap;
                a.state            = ArcState::Gap;
                a.ep_gap_started   = epoch_idx_;
                a.ep_state_changed = epoch_idx_;
                a.gap_len_ep       = static_cast<int>(epoch_idx_ - a.ep_last_seen);
                s.gap_len_ep       = a.gap_len_ep;
            } else {   // already Gap
                a.gap_len_ep = static_cast<int>(epoch_idx_ - a.ep_last_seen);
                if (horizon_s_ > 0.0 && (t_s_ - a.t_last_seen_s) > horizon_s_) {
                    s.transition       = ArcTransition::Death;
                    a.state            = ArcState::Dead;
                    a.ep_state_changed = epoch_idx_;
                    a.reason_mask     |= kArcReasonHorizon;
                    s.gap_len_ep       = a.gap_len_ep;
                } else {
                    continue;   // still gapped, no edge
                }
            }
            s.to          = a.state;
            s.generation  = a.generation;
            s.reason_mask = a.reason_mask;
            s.adm_state      = a.adm_state;
            s.adm_gap_len_ep = a.admittedGapEp(epoch_idx_);
            s.adm_generation = a.adm_generation;
            out.emplace_back(key, s);
        }
        for (const auto& k : forget) arcs_.erase(k);
    }

    void ArcLedger::admitEpoch(const std::set<Key>& admitted,
                               std::vector<std::pair<Key, ArcStep>>& out) {
        out.clear();
        // The present set is derived from the admitted rows themselves rather than
        // taken from the caller: the rule that matters is the raw pass's rule (a
        // station that reported nothing must not gap all of its arcs), and on the
        // admitted side "reported nothing" means "contributed no modelled row".
        std::set<int> present;
        for (const auto& k : admitted) present.insert(std::get<0>(k));

        // (1) The arcs admitted at this epoch.  Inserts happen here, before the
        //     sweep below walks the map, so the sweep never sees a half-built record.
        for (const Key& key : admitted) {
            auto it = arcs_.find(key);
            if (it == arcs_.end()) it = arcs_.emplace(key, ArcRecord{}).first;
            ArcRecord& a = it->second;

            ArcStep s;
            s.from = a.adm_state;
            switch (a.adm_state) {
            case ArcState::Absent:
            case ArcState::Dead:
                s.transition      = ArcTransition::Birth;
                s.prev_generation = a.adm_generation;
                s.gap_len_ep      = (a.ep_last_admitted < 0)
                                      ? 0
                                      : static_cast<int>(epoch_idx_ - a.ep_last_admitted - 1);
                ++a.adm_generation;
                if (a.ep_first_admitted < 0) a.ep_first_admitted = epoch_idx_;
                a.adm_state            = ArcState::Tracking;
                a.ep_adm_state_changed = epoch_idx_;
                a.adm_gap_len_ep       = 0;
                a.adm_reason_mask      = kArcReasonNone;
                break;
            case ArcState::Gap:
                s.transition           = ArcTransition::Reentry;
                s.gap_len_ep           = static_cast<int>(epoch_idx_ - a.ep_last_admitted - 1);
                a.adm_gap_len_ep       = s.gap_len_ep;
                a.adm_state            = ArcState::Reentered;
                a.ep_adm_state_changed = epoch_idx_;
                break;
            case ArcState::Reentered:
                a.adm_state            = ArcState::Tracking;
                a.ep_adm_state_changed = epoch_idx_;
                a.adm_gap_len_ep       = 0;
                break;
            case ArcState::Tracking:
                break;
            }
            a.ep_last_admitted  = epoch_idx_;
            a.t_last_admitted_s = t_s_;
            s.to             = a.adm_state;
            s.generation     = a.adm_generation;
            s.reason_mask    = a.adm_reason_mask;
            s.adm_state      = a.adm_state;
            s.adm_gap_len_ep = a.adm_gap_len_ep;
            s.adm_generation = a.adm_generation;
            if (s.transition != ArcTransition::None) out.emplace_back(key, s);
        }

        // (2) Gap and kill the admitted arcs the present stations did not model.
        for (auto& [key, a] : arcs_) {
            if (a.adm_state == ArcState::Absent || a.adm_state == ArcState::Dead) continue;
            if (!present.count(std::get<0>(key))) continue;   // station absent: frozen
            if (admitted.count(key)) continue;                // admitted this epoch

            ArcStep s;
            s.from = a.adm_state;
            if (a.adm_state == ArcState::Tracking || a.adm_state == ArcState::Reentered) {
                s.transition           = ArcTransition::Gap;
                a.adm_state            = ArcState::Gap;
                a.ep_adm_state_changed = epoch_idx_;
                a.adm_gap_len_ep       = static_cast<int>(epoch_idx_ - a.ep_last_admitted);
                s.gap_len_ep           = a.adm_gap_len_ep;
            } else {   // already Gap
                a.adm_gap_len_ep = static_cast<int>(epoch_idx_ - a.ep_last_admitted);
                if (horizon_s_ > 0.0 && (t_s_ - a.t_last_admitted_s) > horizon_s_) {
                    s.transition           = ArcTransition::Death;
                    a.adm_state            = ArcState::Dead;
                    a.ep_adm_state_changed = epoch_idx_;
                    a.adm_reason_mask     |= kArcReasonHorizon;
                    s.gap_len_ep           = a.adm_gap_len_ep;
                } else {
                    continue;   // still gapped, no edge
                }
            }
            s.to             = a.adm_state;
            s.generation     = a.adm_generation;
            s.reason_mask    = a.adm_reason_mask;
            s.adm_state      = a.adm_state;
            s.adm_gap_len_ep = a.adm_gap_len_ep;
            s.adm_generation = a.adm_generation;
            out.emplace_back(key, s);
        }
    }

    bool ArcLedger::eventAllowed(int station, ArcTransition t) {
        // The first epoch's births are the flood NetworkSignalIntegrity.cpp names
        // (about 12 000 rows at 305 stations), and they carry no information: every
        // arc is born there.  The counter carries that volume instead.
        if (t == ArcTransition::Birth && epoch_idx_ <= 0) return false;
        const auto key = std::make_pair(station, static_cast<int>(t));
        auto it = ev_last_.find(key);
        if (it != ev_last_.end()
            && epoch_idx_ - it->second < kArcLedgerEventHoldoffEp) return false;
        ev_last_[key] = epoch_idx_;
        return true;
    }

} // namespace GnssProcessor
