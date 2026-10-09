/**
 * @file ArcLedger.cpp
 * @brief The arc lifecycle state machine (card SID phase 1; see ArcLedger.hpp).
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

    // =====================================================================
    // Reason vocabulary and the per-epoch buffer (card SID phase 4, S1)
    // =====================================================================

    const char* arcReasonName(unsigned bit) {
        switch (bit) {
        case kArcReasonHorizon:   return "horizon";
        case kArcReasonMask:      return "mask";
        case kArcReasonSlip:      return "slip";
        case kArcReasonGate:      return "gate";
        case kArcReasonPrune:     return "prune";
        case kArcReasonAllowlist: return "allowlist";
        case kArcReasonAdmission: return "admission";
        case kArcReasonReadmit:   return "readmit";
        case kArcReasonGross:     return "gross";
        case kArcReasonDeweight:  return "deweight";
        case kArcReasonNoModel:   return "nomodel";
        case kArcReasonUnknown:   return "unknown";
        default: break;
        }
        return "?";
    }

    int arcReasonIndex(unsigned bit) {
        for (int i = 0; i < kArcReasonCount; ++i)
            if (bit == (1u << i)) return i;
        return -1;
    }

    std::string arcReasonMaskName(unsigned mask) {
        std::string out;
        for (int i = 0; i < kArcReasonCount; ++i) {
            const unsigned b = 1u << i;
            if (!(mask & b)) continue;
            if (!out.empty()) out += "|";
            out += arcReasonName(b);
        }
        return out.empty() ? std::string("-") : out;
    }

    void arcReasonBeginEpoch(ArcReasonEpoch& buf, long long ep) {
        buf.late.clear();
        // The late bits (the readmit park, which runs after the fold) are moved to
        // their own map and kept for one epoch, the earliest epoch at which a gap they
        // can explain can open.  Everything else is dropped: an early writer's record
        // has already been folded at its own epoch, and keeping it would attribute
        // the next gap to a screen that did not open it.
        if (buf.armed)
            for (const auto& [k, r] : buf.rec) {
                const unsigned keep = r.bits & kArcReasonLateMask;
                if (keep != 0u) buf.late[k] = keep;
            }
        buf.rec.clear();
        buf.ep = ep;
    }

    void arcReasonRecord(ArcReasonEpoch& buf, const ArcLedger::Key& key,
                         unsigned bit, bool known) {
        if (!buf.armed) return;
        const int idx = arcReasonIndex(bit);
        if (idx < 0) return;
        ++buf.calls[static_cast<std::size_t>(idx)];
        if (!known) ++buf.calls_unknown[static_cast<std::size_t>(idx)];
        ArcReasonRec& r = buf.rec[key];
        r.bits |= bit;
        r.ep    = buf.ep;
    }

    unsigned arcReasonOf(const ArcReasonEpoch& buf, const ArcLedger::Key& key) {
        unsigned m = kArcReasonNone;
        const auto it = buf.rec.find(key);
        if (it != buf.rec.end()) m |= it->second.bits;
        const auto lt = buf.late.find(key);
        if (lt != buf.late.end()) m |= lt->second;
        return m;
    }

    long long arcReasonExpand(ArcReasonEpoch& buf, const ArcLedger& led,
                              int station, const std::string& sat,
                              const std::string& signal, unsigned bit) {
        if (!buf.armed) return 0;
        long long n = 0;
        const auto& arcs = led.arcs();
        // The key orders on the station index first, then the satellite, so one
        // station's arcs of one satellite are a contiguous range and the expansion
        // stays O(arcs of that satellite) rather than O(network).
        // The station bound in the loop is required for correctness.  `lower_bound`
        // on (station, sat, "") returns the first key at or after it, which for a
        // station holding no arc of `sat` can be the first key of a later station;
        // if that station holds `sat`, an unbounded scan would write the bit on
        // another station's arcs.  The satellite test alone does not catch this,
        // because the satellite matches there.
        const auto scan = [&](int want_sta) {
            for (auto it = arcs.lower_bound(ArcLedger::Key{ want_sta, sat, "" });
                 it != arcs.end(); ++it) {
                if (std::get<0>(it->first) != want_sta) break;
                if (std::get<1>(it->first) != sat) break;
                if (!signal.empty() && std::get<2>(it->first) != signal) continue;
                if (it->second.adm_state != ArcState::Tracking
                    && it->second.adm_state != ArcState::Reentered) continue;
                arcReasonRecord(buf, it->first, bit, true);
                ++n;
            }
        };
        if (station >= 0) {
            scan(station);
        } else {
            for (auto it = arcs.begin(); it != arcs.end(); ) {
                const int si = std::get<0>(it->first);
                scan(si);
                // Advance to the first key of the next station.
                it = arcs.lower_bound(ArcLedger::Key{ si + 1, "", "" });
            }
        }
        if (n == 0) ++buf.calls_unknown[static_cast<std::size_t>(
                std::max(0, arcReasonIndex(bit)))];
        return n;
    }

    const char* arcReentryOutcomeName(ArcReentryOutcome o) {
        switch (o) {
        case ArcReentryOutcome::None:             return "none";
        case ArcReentryOutcome::Freed:            return "freed";
        case ArcReentryOutcome::BridgedVerdict:   return "bridged_verdict";
        case ArcReentryOutcome::BridgedNoVerdict: return "bridged_no_verdict";
        case ArcReentryOutcome::NoCarry:          return "no_carry";
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
        auto it = arcs_.find(key);
        if (it == arcs_.end()) it = arcs_.emplace(key, ArcRecord{}).first;
        ArcRecord& a = it->second;

        ArcStep s;
        s.from = a.state;
        switch (a.state) {
        case ArcState::Absent:
        case ArcState::Dead:
            // A birth.  The previous generation is returned on the step so a caller can
            // tell a true first sighting (0) from a return after a death, a distinction
            // the anchor election does not make.
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
            // A re-entry inside the horizon.  The gap length is the number of epochs
            // missed, the unit every existing re-entry instrument reports.
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
        // The admitted side as it stands at this epoch (card SID phase 2), so a raw
        // re-entry event states both gap lengths.  This epoch's admitted pass runs
        // later (downstream), so the number is the admitted gap that is open as the
        // raw arc returns, which is the quantity a reader of the raw edge needs.
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
                // The forget sweep runs at every station, present or not: a record that
                // has been dead for the whole window carries no information a station's
                // return could use.
                if (forget_ep_ > 0 && epoch_idx_ - a.ep_state_changed > forget_ep_)
                    forget.push_back(key);
                continue;
            }
            if (!present_stations.count(sta)) continue;   // station absent: frozen
            // Observed this epoch: observe() stamps ep_last_seen with the open
            // epoch's index, and nothing else writes it, so the stamp is the
            // record of the sighting (the epoch index strictly increases from one
            // beginEpoch to the next).  It replaces a per-epoch set of the observed
            // keys that held the same fact at the cost of one tree insert per
            // observation and one lookup per arc (card NOM step N3a, where the
            // ledger became part of every run).
            if (a.ep_last_seen == epoch_idx_) continue;

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
                               std::vector<std::pair<Key, ArcStep>>& out,
                               const ArcReasonEpoch* reasons) {
        out.clear();
        // The fold (card SID phase 4, S1).  A gap opened by this call takes the reasons
        // the screens recorded for its arc, narrowed to the bits that can explain a
        // removal (a deweight is not one), or `unknown` when none did.  Unarmed, or
        // with no buffer, the mask stays as phase 3 wrote it.
        const bool fold = reasons != nullptr && reasons->armed;
        const auto gapReason = [&](const Key& key) -> unsigned {
            if (!fold) return kArcReasonNone;
            const unsigned m = arcReasonOf(*reasons, key) & kArcReasonGapMask;
            return m == 0u ? kArcReasonUnknown : m;
        };
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
                s.state_recreated = a.state_pruned;
                a.state_pruned    = false;
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
                // The column this return lands on is a fresh one when the prune
                // destroyed it inside the gap (card SID phase 3).  Reported on the edge
                // and then cleared, so the mark describes one gap and does not carry
                // into the next.
                s.state_recreated      = a.state_pruned;
                a.state_pruned         = false;
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
            // Admitted this epoch: step (1) stamped ep_last_admitted with the open
            // epoch's index, the only write to it, so the stamp answers what a
            // lookup in `admitted` would (this call runs once per epoch; card NOM
            // step N3a, as for the raw side's sighting stamp).
            if (a.ep_last_admitted == epoch_idx_) continue;

            ArcStep s;
            s.from = a.adm_state;
            if (a.adm_state == ArcState::Tracking || a.adm_state == ArcState::Reentered) {
                s.transition           = ArcTransition::Gap;
                a.adm_state            = ArcState::Gap;
                a.ep_adm_state_changed = epoch_idx_;
                a.adm_gap_len_ep       = static_cast<int>(epoch_idx_ - a.ep_last_admitted);
                s.gap_len_ep           = a.adm_gap_len_ep;
                // Stamp the reason (S1).  The mask lasts through the whole gap and is
                // still readable at the re-entry, which lets phase 3's no-verdict
                // population be split by the screen that opened its gap.
                a.adm_reason_mask      = gapReason(key);
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

    bool ArcLedger::markStatePruned(const Key& key) {
        const auto it = arcs_.find(key);
        if (it == arcs_.end()) return false;
        ArcRecord& a = it->second;
        a.state_pruned    = true;
        a.ep_state_pruned = epoch_idx_;
        ++a.n_state_pruned;
        return true;
    }

    void arcLedgerReentryPopulation(const ArcLedger& led,
                                    const std::set<ArcLedger::Key>& admitted,
                                    long long ep, double now_s, double max_gap_s,
                                    ArcReentryEpoch& out) {
        out.arcs.clear();
        for (const ArcLedger::Key& key : admitted) {
            const ArcRecord* a = led.find(key);
            if (a == nullptr) continue;              // first sighting: not a return
            if (a->ep_last_admitted < 0) continue;   // never admitted: not a return
            const long long gap = ep - a->ep_last_admitted - 1;
            if (gap < 1) continue;                   // admitted last epoch: no gap
            const double gap_s = now_s - a->t_last_admitted_s;
            // The horizon is the carry's, not the ledger's: past it, the PASS-B inject
            // would not fold the level across the break either, so there is nothing for
            // the response to protect and nothing to report as missing.
            if (max_gap_s > 0.0 && (gap_s < 0.0 || gap_s > max_gap_s)) {
                ++out.outside_horizon;
                continue;
            }
            ArcReentryArc r;
            r.station         = std::get<0>(key);
            r.satellite       = std::get<1>(key);
            r.signal          = std::get<2>(key);
            r.gap_ep          = static_cast<int>(gap);
            r.gap_s           = gap_s;
            r.state_recreated = a->state_pruned;
            out.arcs.push_back(std::move(r));
        }
    }

    bool ArcLedger::eventAllowed(int station, ArcTransition t) {
        // The first epoch's births are the volume NetworkSignalIntegrity.cpp names
        // (about 12 000 rows at 305 stations), and they carry no information: every
        // arc is born there.  The counter records that volume instead.
        if (t == ArcTransition::Birth && epoch_idx_ <= 0) return false;
        const auto key = std::make_pair(station, static_cast<int>(t));
        auto it = ev_last_.find(key);
        if (it != ev_last_.end()
            && epoch_idx_ - it->second < kArcLedgerEventHoldoffEp) return false;
        ev_last_[key] = epoch_idx_;
        return true;
    }

} // namespace GnssProcessor
