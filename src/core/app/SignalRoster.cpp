/**
 * @file SignalRoster.cpp
 * @brief The signal roster, its builders and its observatory (card SID, from phase 1;
 *        see SignalRoster.hpp).
 */
#include "app/SignalRoster.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"        // NetStation (roster + its PppModel / StateManager)
#include "filter/StateKeySchema.hpp"         // parseAmbiguityKey, for the prune's keys (card SID phase 3)
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"
#include "model/PppModel.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/NetworkRawObservables.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace GnssProcessor {

    namespace {

        /// @brief Is this a usable three-or-more character code or phase key?
        bool isObsKey(const std::string& k, char type) {
            return k.size() >= 3 && k[0] == type && k[1] >= '0' && k[1] <= '9';
        }

        /// @brief The matching code key of a phase key ("L1W" gives "C1W").
        std::string codeOf(const std::string& phase) { return "C" + phase.substr(1); }

        /// @brief One scoped wall-clock accumulator for the two seams' own timers.
        class Stopwatch {
        public:
            explicit Stopwatch(double& sink)
                : sink_(sink), t0_(std::chrono::steady_clock::now()) {}
            ~Stopwatch() {
                sink_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0_).count();
            }
            Stopwatch(const Stopwatch&) = delete;
            Stopwatch& operator=(const Stopwatch&) = delete;
        private:
            double& sink_;
            std::chrono::steady_clock::time_point t0_;
        };

        /// @brief The family key the phase-bias support census and the wire use.
        std::string familyKey(const std::string& sat, const std::string& signal) {
            return sat + "|" + signal;
        }

        /// @brief Median of a sample, by value (the sample is copied and sorted).
        double medianOf(std::vector<int> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const std::size_t n = v.size();
            return (n % 2) ? static_cast<double>(v[n / 2])
                           : 0.5 * (static_cast<double>(v[n / 2 - 1])
                                    + static_cast<double>(v[n / 2]));
        }

    }  // namespace

    const char* signalRoleName(SignalRole r) {
        switch (r) {
        case SignalRole::Tracked:   return "tracked";
        case SignalRole::Modelled:  return "modelled";
        case SignalRole::Datum:     return "datum";
        case SignalRole::Companion: return "companion";
        case SignalRole::Excluded:  return "excluded";
        }
        return "?";
    }

    // =====================================================================
    // The pure builders
    // =====================================================================

    std::map<char, BandRoster> signalRosterBuildCells(
            const std::vector<std::string>& tracked,
            const std::vector<RosterLatchRow>& latch,
            const std::vector<std::string>& companion_allow,
            bool companions_armed,
            const std::set<std::string>* allowlist,
            const std::vector<std::string>& second_phase_allow,
            bool second_phase_armed,
            bool all_channels) {
        std::map<char, BandRoster> out;

        // 1. The tracked sets, per band digit, deduplicated and ascending.
        for (const std::string& k : tracked) {
            if (isObsKey(k, 'L'))      out[k[1]].tracked_phase.push_back(k);
            else if (isObsKey(k, 'C')) out[k[1]].tracked_code.push_back(k);
        }
        for (auto& [band, br] : out) {
            (void)band;
            for (auto* v : { &br.tracked_phase, &br.tracked_code }) {
                std::sort(v->begin(), v->end());
                v->erase(std::unique(v->begin(), v->end()), v->end());
            }
        }

        // 2. The modelled roles, copied from the latch and never re-derived.  A band
        //    the latch names but the receiver does not report this epoch still gets
        //    its cell: the latch is the estimator's key, so the cell states which
        //    signal the station carries on that band on the epochs the arc is absent
        //    as well.
        for (const RosterLatchRow& r : latch) {
            if (r.signal.size() < 2) continue;
            const char band = r.signal[1];
            if (r.obs_type == 'L')      out[band].modelled_phase = r.signal;
            else if (r.obs_type == 'C') out[band].modelled_code  = r.signal;
        }

        // 3. The companion codes (card SCC).  Band 1 only, the allow list walked in
        //    its configured order, skipping the canonical code, untracked codes, codes
        //    outside the allowlist and duplicates, with the canonical code required
        //    present this epoch.  NetworkSecondCodeScope.cpp and PppModel implement
        //    the same rule.
        if (companions_armed) {
            auto bit = out.find('1');
            if (bit != out.end()) {
                BandRoster& b1 = bit->second;
                const bool canon_present =
                    !b1.modelled_code.empty()
                    && std::binary_search(b1.tracked_code.begin(), b1.tracked_code.end(),
                                          b1.modelled_code);
                if (canon_present) {
                    for (const std::string& sig : companion_allow) {
                        if (sig.size() != 3 || sig[0] != 'C' || sig[1] != '1') continue;
                        if (sig == b1.modelled_code) continue;
                        if (!std::binary_search(b1.tracked_code.begin(),
                                                b1.tracked_code.end(), sig)) continue;
                        if (allowlist && !allowlist->empty() && !allowlist->count(sig)) continue;
                        if (std::find(b1.companion_code.begin(), b1.companion_code.end(), sig)
                            != b1.companion_code.end()) continue;
                        b1.companion_code.push_back(sig);
                    }
                }
            }
        }

        // 3b. The second phases (card SID phase 6a).  The same walk as the companion
        //     codes above and the rule NetworkSecondPhaseScope.cpp and PppModel
        //     implement: band 1 only, the allow list in its configured order, skipping
        //     the latched phase, untracked phases, phases outside the allowlist and
        //     duplicates, with the latched phase required present this epoch
        //     (otherwise it is the fallback case, which adds nothing).
        if (second_phase_armed) {
            auto bit = out.find('1');
            if (bit != out.end()) {
                BandRoster& b1 = bit->second;
                const bool latched_present =
                    !b1.modelled_phase.empty()
                    && std::binary_search(b1.tracked_phase.begin(), b1.tracked_phase.end(),
                                          b1.modelled_phase);
                if (latched_present) {
                    for (const std::string& sig : second_phase_allow) {
                        if (sig.size() != 3 || sig[0] != 'L' || sig[1] != '1') continue;
                        if (sig == b1.modelled_phase) continue;
                        if (!std::binary_search(b1.tracked_phase.begin(),
                                                b1.tracked_phase.end(), sig)) continue;
                        if (allowlist && !allowlist->empty() && !allowlist->count(sig)) continue;
                        if (std::find(b1.modelled_extra.begin(), b1.modelled_extra.end(), sig)
                            != b1.modelled_extra.end()) continue;
                        b1.modelled_extra.push_back(sig);
                    }
                }
            }
        }

        // 3c. The all-channel widening (card SID phase 6b).  It subsumes 3 and 3b, so it
        //     runs in their place (the caller passes their flags as false) and over
        //     every band rather than band 1 alone: every tracked, admitted phase of a
        //     band other than its modelled one joins `modelled_extra`, and every
        //     tracked, admitted code other than its modelled one joins
        //     `companion_code`.  That is the row set PppModel::selectUncombinedSignals
        //     adds.  The modelled row must be tracked this epoch for any extra row to
        //     be added, the same clause steps 3 and 3b apply.
        if (all_channels) {
            for (auto& [band, br] : out) {
                (void)band;
                const auto take = [&](const std::vector<std::string>& tracked,
                                      const std::string& modelled,
                                      std::vector<std::string>& dst) {
                    if (modelled.empty()) return;
                    if (!std::binary_search(tracked.begin(), tracked.end(), modelled))
                        return;
                    for (const std::string& sig : tracked) {
                        if (sig == modelled) continue;
                        if (allowlist && !allowlist->empty() && !allowlist->count(sig))
                            continue;
                        if (std::find(dst.begin(), dst.end(), sig) != dst.end()) continue;
                        dst.push_back(sig);
                    }
                };
                take(br.tracked_phase, br.modelled_phase, br.modelled_extra);
                take(br.tracked_code,  br.modelled_code,  br.companion_code);
            }
        }

        // 4. The excluded signals, each with the reason that applies.  The allowlist
        //    is the earlier gate, so a signal outside it reads "allowlist"; on a band
        //    the latch carries, any other signal reads "not_latched" (the latch chose
        //    another one); on a band with no latch, the reason is "no_latch".
        for (auto& [band, br] : out) {
            (void)band;
            const auto note = [&](const std::string& sig, const std::string& modelled,
                                  bool companion) {
                if (sig == modelled) return;
                if (companion) return;   // a companion code or a second phase is modelled
                const char* reason = "no_latch";
                if (allowlist && !allowlist->empty() && !allowlist->count(sig))
                    reason = "allowlist";
                else if (!modelled.empty())
                    reason = "not_latched";
                br.excluded.emplace_back(sig, reason);
            };
            for (const std::string& s : br.tracked_phase)
                note(s, br.modelled_phase,
                     std::find(br.modelled_extra.begin(), br.modelled_extra.end(), s)
                         != br.modelled_extra.end());
            for (const std::string& s : br.tracked_code)
                note(s, br.modelled_code,
                     std::find(br.companion_code.begin(), br.companion_code.end(), s)
                         != br.companion_code.end());
        }
        return out;
    }

    std::map<char, std::string> signalRosterTrackerPicks(
            const std::vector<std::string>& tracked) {
        std::set<std::string> keys(tracked.begin(), tracked.end());
        std::map<char, std::string> out;
        // The tracker keeps a phase only when its matching code is reported too, and
        // then takes the lexically lowest such phase of the band, which is what the
        // band-digit-only comparator under std::sort produces on this toolchain.
        // std::set iterates ascending, so the first hit per band is that signal.
        for (const std::string& k : keys) {
            if (!isObsKey(k, 'L')) continue;
            if (!keys.count(codeOf(k))) continue;
            out.emplace(k[1], k);
        }
        return out;
    }

    // =====================================================================
    // Phase 2: the read side, and the IF pin as a writer
    // =====================================================================

    std::string signalRosterModelledPhase(const SignalRoster& rs, int station,
                                          const std::string& cons, char band) {
        if (station < 0 || station >= static_cast<int>(rs.by_station.size()))
            return std::string{};
        const StationRoster& sr = rs.by_station[static_cast<std::size_t>(station)];
        const auto cit = sr.find(cons);
        if (cit == sr.end()) return std::string{};
        const auto bit = cit->second.find(band);
        if (bit == cit->second.end()) return std::string{};
        return bit->second.modelled_phase;
    }

    void signalRosterAdmittedArcs(
            const std::vector<std::vector<PppObservation>>& modeled,
            const std::vector<bool>& present,
            std::set<ArcLedger::Key>& out) {
        out.clear();
        for (std::size_t i = 0; i < modeled.size(); ++i) {
            if (i < present.size() && !present[i]) continue;
            for (const PppObservation& o : modeled[i]) {
                if (o.satellite.empty()) continue;
                for (const auto& [key, val] : o.observed) {
                    (void)val;
                    if (key.size() < 3 || key[0] != 'L') continue;
                    out.emplace(static_cast<int>(i), o.satellite, key);
                }
            }
        }
    }

    void signalRosterModelledArcs(
            const SignalRoster& rs,
            const std::vector<std::vector<PppObservation>>& modeled,
            const std::vector<bool>& present,
            std::set<ArcLedger::Key>& out,
            long long* n_not_modelled) {
        out.clear();
        for (std::size_t i = 0; i < modeled.size(); ++i) {
            if (i < present.size() && !present[i]) continue;
            for (const PppObservation& o : modeled[i]) {
                if (o.satellite.empty()) continue;
                const std::string cons = PppUtils::getSystemString(o.system);
                for (const auto& [key, val] : o.observed) {
                    (void)val;
                    if (key.size() < 3 || key[0] != 'L') continue;
                    // The role decides, not the presence of a row: an arc the
                    // roster does not mark Modelled on this band has no carried
                    // level for the response to protect.
                    if (signalRosterModelledPhase(rs, static_cast<int>(i), cons, key[1])
                        != key) {
                        if (n_not_modelled != nullptr) ++(*n_not_modelled);
                        continue;
                    }
                    out.emplace(static_cast<int>(i), o.satellite, key);
                }
            }
        }
    }

    std::map<std::string, std::string> signalRosterDatumPicks(
            const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real,
            std::map<std::string, std::map<std::string, long long>>* votes_out,
            std::set<std::string>* ambiguous) {
        // Tally the per-satellite pins into (constellation, band) votes.
        //
        // The constellation key is the roster's, not the S-basis one.  The datum seam
        // keys a constellation by the first character of the satellite id ("G"); the
        // roster keys it by the system string PppUtils::getSystemString gives ("GPS"),
        // which is also the key of the signal-priority map the latch is decided from.
        // Votes tallied under the letter would match no roster cell, and the census
        // would read zero.
        std::map<std::string, std::map<std::string, long long>> votes;
        for (const auto& [sat, pair] : if_pin_real) {
            if (sat.empty()) continue;
            const std::string cons = PppUtils::getSystemString(sat);
            if (cons.empty() || cons == "Unknown") continue;
            for (const std::string* sig : { &pair.first, &pair.second }) {
                if (sig->size() < 2 || (*sig)[0] != 'C') continue;
                ++votes[cons + "|" + std::string(1, (*sig)[1])][*sig];
            }
        }
        // Resolve each (constellation, band) to one designation, and mark it ambiguous
        // when the satellites disagree.  std::map iterates ascending and the
        // replacement needs a strictly greater count, so a tie goes to the lexically
        // lowest code, the same tie break the pin applies within a band.
        std::map<std::string, std::string> pick;
        for (const auto& [cb, v] : votes) {
            long long best = -1;
            for (const auto& [sig, n] : v)
                if (n > best) { best = n; pick[cb] = sig; }
            if (v.size() > 1 && ambiguous != nullptr) ambiguous->insert(cb);
        }
        if (votes_out != nullptr) *votes_out = std::move(votes);
        return pick;
    }

    SignalRosterDatumWrite signalRosterApplyDatum(
            StationRoster& sr, const std::map<std::string, std::string>& picks,
            const std::set<std::string>* ambiguous) {
        SignalRosterDatumWrite out;
        for (auto& [cons, cells] : sr) {
            for (auto& [band, br] : cells) {
                const std::string cb = cons + "|" + std::string(1, band);
                const auto pit = picks.find(cb);
                if (pit == picks.end()) continue;
                br.datum_code = pit->second;
                ++out.cells;
                if (ambiguous != nullptr && ambiguous->count(cb)) ++out.ambiguous;
                // The pin names a code.  A datum phase is named only where that code
                // is this station's modelled code, and then it is this cell's
                // modelled phase; elsewhere the roster leaves the phase empty rather
                // than deriving a signal from the code's attribute (the roster
                // reproduces the latch and never re-derives it).
                if (!br.modelled_code.empty() && br.modelled_code == br.datum_code
                    && !br.modelled_phase.empty()) {
                    br.datum_phase = br.modelled_phase;
                    ++out.with_phase;
                } else {
                    ++out.off_model;
                }
            }
        }
        return out;
    }

    void networkSignalRosterDatumPin(
            NetworkRunContext& ctx,
            const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;
        rs.datum_pin.clear();
        rs.datum_votes.clear();
        if (if_pin_real.empty()) return;
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.audit_ms);
        ++ctr.datum_epochs;
        ctr.datum_sat_pins += static_cast<long long>(if_pin_real.size());
        rs.datum_pin = if_pin_real;

        std::set<std::string> ambiguous;
        const auto picks =
            signalRosterDatumPicks(if_pin_real, &rs.datum_votes, &ambiguous);

        for (std::size_t i = 0; i < rs.by_station.size(); ++i) {
            const auto w = signalRosterApplyDatum(rs.by_station[i], picks, &ambiguous);
            ctr.datum_cells           += w.cells;
            ctr.datum_cells_phase     += w.with_phase;
            ctr.datum_cells_off_model += w.off_model;
            ctr.datum_cells_ambiguous += w.ambiguous;
            for (const auto& [cons, cells] : rs.by_station[i]) {
                for (const auto& [band, br] : cells) {
                    const std::string cb = cons + "|" + std::string(1, band);
                    if (!br.datum_code.empty())  ++ctr.datum_code_census[cb + "|" + br.datum_code];
                    if (!br.datum_phase.empty()) ++ctr.datum_phase_census[cb + "|" + br.datum_phase];
                }
            }
        }
    }

    // =====================================================================
    // Arming
    // =====================================================================

    void networkSignalRosterArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations) {
        auto& rs = ctx.roster;
        rs.armed = config_.networkSignalRoster();
        if (!rs.armed) return;

        const auto& nc = config_.getNetworkConfig();
        rs.horizon_pl_s      = config_.networkPhaseLevelingMaxGapS();
        rs.horizon_iono_s    = config_.networkIonoCarryMaxGapS();
        rs.horizon_pbhold_s  = nc.emit_pb_hold_max_gap_s;
        rs.horizon_readmit_s = config_.networkReadmitSeedHorizonS();
        rs.horizon_s = std::max(std::max(rs.horizon_pl_s, rs.horizon_iono_s),
                                std::max(rs.horizon_pbhold_s, rs.horizon_readmit_s));

        const std::size_t n = stations.size();
        rs.by_station.assign(n, {});
        rs.latch_cache.assign(n, {});
        rs.rule_done.assign(n, {});
        rs.station_names.clear();
        rs.station_names.reserve(n);
        for (const auto& st : stations) rs.station_names.push_back(st.name);

        ctx.arc_ledger.configure(rs.horizon_s, kArcLedgerForgetEp);

        // Arm the reason writers (phase 4a, S1).  The screens touch only the buffer,
        // so an unarmed run pays one bool test per writer call and nothing else; the
        // fold then receives an unarmed buffer and leaves the ledger's masks as
        // phase 3 wrote them.
        ctx.reasons.armed = config_.networkLedgerScreens();

        // Arm the ambiguity prune's key recorder on every station (phase 3, R3b), so
        // the audit pass can mark the arcs whose column the prune removed inside a
        // gap.  The prune itself is unchanged and the recorder is a per-epoch vector
        // of strings; it is armed only together with the observatory.
        for (const auto& st : stations)
            if (st.sm) st.sm->setRecordPrunedAmbiguities(true);

        log->info("Network (card SID phase 1 / SIGNAL ROSTER + ARC LEDGER): ARMED "
                  "(network.signal_roster).  OBSERVATORY only: the roster and the "
                  "ledger are built once per epoch and read by NOTHING but the "
                  "disagreement counters and the trace, so the solve, the wire and "
                  "every graded trace are byte-identical against an unarmed run.  "
                  "{} producer station(s).  Ledger horizon {:.0f} s = max("
                  "phase_leveling_max_gap_s {:.0f}, iono_carry_max_gap_s {:.0f}, "
                  "emit_pb_hold_max_gap_s {:.0f}, readmit_seed_horizon_s {:.0f}); "
                  "forget window {} processed epoch(s); ARC event holdoff {} epoch(s) "
                  "per (station, transition), births suppressed on the first epoch.",
                  n, rs.horizon_s, rs.horizon_pl_s, rs.horizon_iono_s,
                  rs.horizon_pbhold_s, rs.horizon_readmit_s,
                  kArcLedgerForgetEp, kArcLedgerEventHoldoffEp);
        if (ctx.reasons.armed)
            log->info("Network (card SID phase 4a / LEDGER SCREENS): ARMED "
                      "(network.ledger_screens).  Every removal screen records the arcs "
                      "it removed and its reason bit into the per-epoch buffer, and the "
                      "audit pass folds the buffer into the admitted gap it opened; a "
                      "gap no writer explains carries 'unknown'.  A de-weight is recorded "
                      "as 'deweight' and never counted as a gap.  The anchor election is "
                      "READ against the ledger at its first-sighting branch.  "
                      "OBSERVATORY: the solve is byte-identical.{}",
                      config_.networkLedgerAnchor()
                          ? "  ⚠ network.ledger_anchor (phase 4b) is ALSO armed, so the "
                            "election ACTS on that read and the run is SOLVE-AFFECTING."
                          : "");
    }

    // =====================================================================
    // The per-epoch build
    // =====================================================================

    void networkBuildSignalRoster(NetworkRunContext& ctx, const GnssConfig& config_,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  NetworkRunTraces& traces,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;                 // byte-identical path: no walk at all
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.build_ms);

        const long long epoch_idx = rs.epochs++;
        ++ctr.epochs;
        const double t_s = TimeUtils::continuousGpsSeconds(se.time);
        ctx.arc_ledger.beginEpoch(epoch_idx, t_s);
        // Open the epoch's reason buffer (phase 4a, S1).  It opens here, at the first
        // seam of the epoch, because the writers run between this seam and the audit
        // pass that folds the buffer (the allowlist writer below and the screens,
        // the gross-LOS screen included), so their bits reach the fold of their own
        // epoch.  The one writer downstream of the audit is the readmit park;
        // arcReasonBeginEpoch keeps its bits for one more epoch, the earliest epoch
        // at which a gap they can explain can open.
        arcReasonBeginEpoch(ctx.reasons, epoch_idx);
        // The response's per-arc record is epoch-scoped (phase 3, R4).  It is cleared
        // at the first seam of the epoch so it never describes a previous epoch.
        ctx.carry.sid_reentry_outcome.clear();

        const auto& nc = config_.getNetworkConfig();
        // The all-channel widening subsumes both cards (card SID phase 6b), so the two
        // allow-list walks stand down and the all-channel walk fills the same two fields.
        const bool all_channels = config_.networkChannelModeAll();
        const bool companions_armed = config_.networkSecondCodeChannel() && !all_channels;
        const std::vector<std::string>& companion_allow = config_.networkSecondCodeSignals();
        const bool second_phase_armed = config_.networkSecondPhaseChannel() && !all_channels;
        const std::vector<std::string>& second_phase_allow =
            config_.networkSecondPhaseSignals();
        const auto& prio = config_.getSignalPriorities();
        (void)nc;

        std::set<int> present_stations;

        for (std::size_t i = 0; i < stations.size() && i < rs.by_station.size(); ++i) {
            StationRoster& sr = rs.by_station[i];
            sr.clear();
            const auto sit = se.by_station.find(stations[i].name);
            if (sit == se.by_station.end() || sit->second.empty()) continue;
            present_stations.insert(static_cast<int>(i));
            const PppModel* model = stations[i].model.get();

            // Group this epoch's raw keys by constellation.  A constellation with no
            // priority list is skipped: the latch's candidate pool is that list, so
            // such a constellation produces no canonicals, and the roster would
            // describe signals the producer never models.  The RINEX files carry every
            // constellation the receiver tracked, so without this skip a GPS-only
            // posture would get cells (and arcs) for GLONASS, Galileo and BeiDou,
            // which is wrong and is the dominant cost.
            std::map<std::string, std::vector<std::string>> tracked_by_cons;
            std::map<std::string, std::vector<const RinexObservation*>> obs_by_cons;
            for (const RinexObservation& o : sit->second) {
                const std::string cons = PppUtils::getSystemString(o.system);
                if (cons.empty() || cons == "Unknown") continue;
                if (!prio.count(cons)) continue;
                auto& tr = tracked_by_cons[cons];
                for (const auto& [key, val] : o.measurements) {
                    (void)val;
                    if (isObsKey(key, 'L') || isObsKey(key, 'C')) tr.push_back(key);
                }
                obs_by_cons[cons].push_back(&o);
            }

            for (auto& [cons, tracked] : tracked_by_cons) {
                // The latch is decided once per constellation and never revised, so
                // the cache is filled only while it is still empty.  The latch for a
                // constellation's first epoch is decided inside computeModels,
                // downstream of this seam, so that epoch has no latch and its cells
                // carry Tracked roles only.
                auto& cache = rs.latch_cache[i];
                auto lit = cache.find(cons);
                if (lit == cache.end() && model) {
                    std::vector<RosterLatchRow> rows;
                    for (const auto& e : model->exportCanonicalSignals()) {
                        if (e.constellation != cons || e.signal.size() < 2) continue;
                        if (e.obs_type != 'C' && e.obs_type != 'L') continue;
                        rows.push_back(RosterLatchRow{ e.obs_type, e.signal });
                    }
                    if (!rows.empty()) lit = cache.emplace(cons, std::move(rows)).first;
                }
                static const std::vector<RosterLatchRow> kNoLatch;
                const std::vector<RosterLatchRow>& latch =
                    (lit == cache.end()) ? kNoLatch : lit->second;

                const std::set<std::string>* allow = nullptr;
                if (model) {
                    const auto& al = model->signalAllowlist();
                    const auto ait = al.find(cons);
                    if (ait != al.end() && !ait->second.empty()) allow = &ait->second;
                }

                auto cells = signalRosterBuildCells(tracked, latch, companion_allow,
                                                    companions_armed, allow,
                                                    second_phase_allow,
                                                    second_phase_armed, all_channels);

                // Self-check (P2).  The roster is built from the latch, so a nonzero
                // count here is a bug in this file, not a finding about the run.
                if (model) {
                    for (const auto& [band, br] : cells) {
                        const std::string lp =
                            model->latchedCanonicalPhaseOnBandDigit(cons, band);
                        const std::string lc =
                            model->latchedCanonicalCodeOnBandDigit(cons, band);
                        if (br.modelled_phase != lp || br.modelled_code != lc)
                            ++ctr.roster_latch_disagree;
                    }
                }

                for (const auto& [band, br] : cells) {
                    (void)band;
                    ++ctr.cells;
                    if (!br.modelled_phase.empty() || !br.modelled_code.empty())
                        ++ctr.cells_modelled;
                    ctr.tracked_signals +=
                        static_cast<long long>(br.tracked_phase.size() + br.tracked_code.size());
                    ctr.companion_signals += static_cast<long long>(br.companion_code.size());
                    for (const auto& [sig, reason] : br.excluded) {
                        ++ctr.excluded_signals;
                        const std::string r = reason;
                        if (r == "allowlist")        ++ctr.excl_allowlist;
                        else if (r == "not_latched") ++ctr.excl_not_latched;
                        else                         ++ctr.excl_no_latch;
                        // The network signal allowlist writer (phase 4a, S1).  The
                        // decision is taken once per run inside PppModel, which
                        // carries no station identity, but the roster has already
                        // reproduced it per (station, constellation, band, signal)
                        // with its reason, so the writer sits here rather than in a
                        // shared model.  The allowlist is a whole-run exclusion, so
                        // it removes arcs that were never admitted and the expansion
                        // normally reaches nothing; that zero is counted in
                        // calls_unknown.
                        if (!ctx.reasons.armed || r != "allowlist") continue;
                        long long n_hit = 0;
                        const auto& arcs = ctx.arc_ledger.arcs();
                        for (auto ait = arcs.lower_bound(
                                 ArcLedger::Key{ static_cast<int>(i), "", "" });
                             ait != arcs.end()
                             && std::get<0>(ait->first) == static_cast<int>(i); ++ait) {
                            if (std::get<2>(ait->first) != sig) continue;
                            if (ait->second.adm_state != ArcState::Tracking
                                && ait->second.adm_state != ArcState::Reentered) continue;
                            arcReasonRecord(ctx.reasons, ait->first, kArcReasonAllowlist);
                            ++n_hit;
                        }
                        if (n_hit == 0)
                            ++ctx.reasons.calls_unknown[static_cast<std::size_t>(
                                arcReasonIndex(kArcReasonAllowlist))];
                    }
                }

                // Class (b): the count rule against the priority rule, run once per
                // (station, constellation) as soon as the candidate list exists.
                // A constellation with no candidate list is not comparable yet (the
                // latch is decided inside computeModels, downstream of this seam),
                // and the roster sees every constellation in the raw record,
                // including ones this run does not process.  Reachability is
                // therefore tracked positively, per station: a station counts as
                // reached once one of its constellations has been compared.
                if (!rs.rule_done[i].count(cons) && model) {
                    const auto& cand_map = model->canonicalCandidates();
                    const auto cit = cand_map.find(cons);
                    if (cit != cand_map.end() && !cit->second.empty()) {
                        rs.rule_done[i].insert(cons);
                        rs.rule_reached.insert(stations[i].name);
                        const double cov = config_.canonicalSignalMinCoverage();
                        const bool follows = config_.canonicalSignalCodeFollowsPhase();
                        const auto p = PppModel::decideCanonicalPicks(
                            cit->second, CanonicalSignalRule::Priority, cov, follows);
                        const auto c = PppModel::decideCanonicalPicks(
                            cit->second, CanonicalSignalRule::Count, cov, follows);
                        for (const auto& [key, pick] : p) {
                            const auto cc = c.find(key);
                            if (cc != c.end() && cc->second.signal == pick.signal) continue;
                            rs.rule_differs.insert(stations[i].name);
                            // The population of card CSL is the band-1 phase, the pick
                            // the estimator depends on; the wider count over any
                            // (band, obs type) is reported beside it.
                            if (key.second == 'L' && !pick.signal.empty()
                                && pick.signal.size() >= 2 && pick.signal[1] == '1')
                                rs.rule_differs_b1.insert(stations[i].name);
                        }
                    }
                }

                // The ledger: every (satellite, signal) this station reported.  The
                // arc is keyed on the signal as well as the satellite (card SID).
                const auto obs_it = obs_by_cons.find(cons);
                if (obs_it != obs_by_cons.end()) {
                    for (const RinexObservation* o : obs_it->second) {
                        if (o->satellite.empty()) continue;
                        for (const auto& [key, val] : o->measurements) {
                            (void)val;
                            if (!isObsKey(key, 'L')) continue;
                            const auto cell = cells.find(key[1]);
                            // A second phase is modelled too (card SID phase 6a): it
                            // carries its own float ambiguity and joins its signal's
                            // satellite phase-bias family, so the ledger's `modelled`
                            // flag reads the extra set beside the latch.  The set is
                            // empty unless the second-phase channel or the all-channel
                            // mode is armed, so every other posture is unchanged.
                            const bool modelled =
                                cell != cells.end()
                                && (cell->second.modelled_phase == key
                                    || std::find(cell->second.modelled_extra.begin(),
                                                 cell->second.modelled_extra.end(), key)
                                           != cell->second.modelled_extra.end());
                            const ArcLedger::Key ak{ static_cast<int>(i), o->satellite, key };
                            const ArcStep step = ctx.arc_ledger.observe(ak, modelled);
                            if (step.transition == ArcTransition::Birth) {
                                ++ctr.arc_birth;
                                if (step.gap_len_ep >= 2 && step.gap_len_ep <= 30)
                                    ++ctr.gap_any;
                            } else if (step.transition == ArcTransition::Reentry) {
                                ++ctr.arc_reentry;
                                ++ctr.gap_any;
                                const bool carried =
                                    ctx.carry.local_carry.count(
                                        std::make_tuple(static_cast<int>(i), o->satellite, key)) > 0;
                                if (carried && step.gap_len_ep >= 2 && step.gap_len_ep <= 30)
                                    ++ctr.gap_2_30;
                            }
                            if (step.transition != ArcTransition::None) {
                                ++ctr.arc_trace_rows;
                                traces.writeArcLedgerRow(
                                    se.time, stations[i].name, o->satellite, key,
                                    arcStateName(step.from), arcStateName(step.to),
                                    step.generation, step.gap_len_ep, "",
                                    modelled ? "modelled" : "tracked",
                                    "raw", arcStateName(step.adm_state),
                                    step.adm_gap_len_ep);
                                if (ctx.arc_ledger.eventAllowed(static_cast<int>(i),
                                                                step.transition)) {
                                    ++ctr.arc_events;
                                    traces.emitEvent(
                                        se.time, "ARC", arcTransitionName(step.transition),
                                        o->satellite, stations[i].name,
                                        static_cast<double>(step.gap_len_ep),
                                        static_cast<double>(step.generation),
                                        arcStateName(step.to),
                                        // Both gap lengths (phase 2).  `gap_ep` is the raw
                                        // one; `adm_gap_ep` is the number of epochs the
                                        // admitted side has missed when this raw edge is
                                        // crossed (-1 when the arc has never been
                                        // admitted).  A carry reader needs the latter, and
                                        // it cannot be derived from the raw one.
                                        fmt::format("signal={};gen={};prev_gen={};gap_ep={};"
                                                    "adm_gap_ep={};adm_state={};role={}",
                                                    key, step.generation, step.prev_generation,
                                                    step.gap_len_ep, step.adm_gap_len_ep,
                                                    arcStateName(step.adm_state),
                                                    modelled ? "modelled" : "tracked"));
                                }
                            }
                        }
                    }
                }
                sr[cons] = std::move(cells);
            }
        }

        // Close the epoch: gap and kill what the present stations did not report.
        std::vector<std::pair<ArcLedger::Key, ArcStep>> edges;
        ctx.arc_ledger.endEpoch(present_stations, edges);
        for (const auto& [key, step] : edges) {
            const int sta = std::get<0>(key);
            const std::string& sat = std::get<1>(key);
            const std::string& sig = std::get<2>(key);
            if (step.transition == ArcTransition::Gap)        ++ctr.arc_gap;
            else if (step.transition == ArcTransition::Death) ++ctr.arc_death;
            const std::string& name =
                (sta >= 0 && sta < static_cast<int>(rs.station_names.size()))
                    ? rs.station_names[static_cast<std::size_t>(sta)] : sat;
            const char* reason =
                (step.reason_mask & kArcReasonHorizon) ? "horizon" : "";
            ++ctr.arc_trace_rows;
            traces.writeArcLedgerRow(se.time, name, sat, sig,
                                     arcStateName(step.from), arcStateName(step.to),
                                     step.generation, step.gap_len_ep, reason, "",
                                     "raw", arcStateName(step.adm_state),
                                     step.adm_gap_len_ep);
            if (ctx.arc_ledger.eventAllowed(sta, step.transition)) {
                ++ctr.arc_events;
                traces.emitEvent(se.time, "ARC", arcTransitionName(step.transition),
                                 sat, name, static_cast<double>(step.gap_len_ep),
                                 static_cast<double>(step.generation),
                                 arcStateName(step.to),
                                 fmt::format("signal={};gen={};gap_ep={};adm_gap_ep={};"
                                             "adm_state={};reason={}",
                                             sig, step.generation, step.gap_len_ep,
                                             step.adm_gap_len_ep,
                                             arcStateName(step.adm_state),
                                             reason[0] ? reason : "-"));
            }
        }

        ctr.arc_live_max = std::max<long long>(
            ctr.arc_live_max, static_cast<long long>(ctx.arc_ledger.liveCount()));
        ctr.arc_known_max = std::max<long long>(
            ctr.arc_known_max, static_cast<long long>(ctx.arc_ledger.size()));
        if (epoch_idx == 0)
            log->debug("Network (card SID): first roster epoch built at TOW {:.1f}: "
                       "{} station(s) present, {} arc(s) born.",
                       se.time.tow, present_stations.size(), ctr.arc_birth);
    }

    // =====================================================================
    // The disagreement pass
    // =====================================================================

    void networkSignalRosterAudit(NetworkRunContext& ctx,
                                  const std::shared_ptr<spdlog::logger>& log,
                                  NetworkRunTraces& traces,
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations,
                                  const std::vector<std::vector<PppObservation>>& modeled,
                                  const std::vector<bool>& present,
                                  const RawSlipEpoch* raw,
                                  const std::set<std::pair<int, std::string>>& freed,
                                  const NetworkSolutionEpoch& sol,
                                  bool emitted) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.audit_ms);
        (void)log;
        // The epoch's continuous-GPS seconds, the unit every carry horizon is in.
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);

        // The answered test, placed above the admitted pass because that pass's
        // census uses it.  The gap re-entry response fires in PASS A, upstream of
        // this pass, and its latch is `ctx.carry.relevel.free_ambs` with the cause
        // bit `kCauseGap` (an entry with no recorded cause also counts).  The
        // driver's slipped set is a wider channel (RAIM, D5 and the deweight release
        // all write to it), so the narrow latch is what shows whether a re-entry was
        // answered by the tier built for it.
        const auto& rel = ctx.carry.relevel;
        const auto answered = [&](int sta, const std::string& sat,
                                  const std::string& sig) {
            const auto k = std::make_pair(sta, sat + "_" + sig);
            if (!rel.free_ambs.count(k)) return false;
            const auto cit = rel.free_cause.find(k);
            return cit == rel.free_cause.end()
                   || (cit->second & RelevelEpochState::kCauseGap) != 0;
        };

        // --- Phase 3 (R3a): the carry's gap prune as an arc death ---------------
        // The prune runs in the harvest seam, which has neither the trace bundle
        // nor the epoch time, so it buffers its keys in the context and they are
        // evented here.  This is the decoupling edge of standing invariant 7 for
        // the carry itself.
        for (const ArcLedger::Key& key : ctx.carry.sid_carry_prune_deaths) {
            ++ctr.carry_prune_deaths;
            const int sta = std::get<0>(key);
            const std::string& sat = std::get<1>(key);
            const std::string& sig = std::get<2>(key);
            const std::string& name =
                (sta >= 0 && sta < static_cast<int>(rs.station_names.size()))
                    ? rs.station_names[static_cast<std::size_t>(sta)] : sat;
            const ArcRecord* a = ctx.arc_ledger.find(key);
            ++ctr.adm_trace_rows;
            traces.writeArcLedgerRow(se.time, name, sat, sig,
                                     a ? arcStateName(a->adm_state) : "absent",
                                     "dead", a ? a->adm_generation : 0,
                                     a ? a->adm_gap_len_ep : 0, "horizon", "carry",
                                     "carry", a ? arcStateName(a->adm_state) : "absent",
                                     a ? a->admittedGapEp(ctx.arc_ledger.epochIndex()) : -1);
            if (ctx.arc_ledger.eventAllowed(sta, ArcTransition::Death)) {
                ++ctr.carry_prune_events;
                traces.emitEvent(se.time, "ARC", "arc_death", sat, name,
                                 static_cast<double>(a ? a->adm_gap_len_ep : 0),
                                 std::nullopt, "carry_prune",
                                 fmt::format("signal={};reason=horizon;src=local_carry;"
                                             "max_gap_s={:.0f};adm_state={}",
                                             sig, rs.horizon_pl_s,
                                             a ? arcStateName(a->adm_state) : "absent"));
            }
        }
        ctx.carry.sid_carry_prune_deaths.clear();

        // --- Phase 2: the admitted lifecycle, before every class that reads it ---
        // The feed is the station's modelled phase rows.  `modeled[i]` exists only
        // for satellites that passed the elevation mask, and `observed` holds only
        // the signals the model built (the canonical latch, its card SCC companions
        // and whatever the allow list admitted), which is the set `local_carry` and
        // the ambiguity states are keyed from.  The raw side, fed at the build seam
        // upstream of all of that, is a strict superset; the two are reported side
        // by side and never added.
        {
            // The slip writer (phase 4a, S1), on the one set every refloat vehicle
            // writes.  `relevel.free_ambs` is written by the MW / LLI drains, the
            // IF-TD tier, the RAIM T3 and T4 responses, the gap re-entry tiers and
            // the de-weight release, each with the exact (station, ambiguity key)
            // identity, so recording it here is one site instead of a bit added at
            // every responder.  It also precedes the fold below, as the mask
            // requires.  A freed arc opens a gap only where the free is followed by
            // the row disappearing; otherwise the bit lands on no gap.
            if (ctx.reasons.armed)
                for (const auto& [sta, amb] : ctx.carry.relevel.free_ambs) {
                    const auto us = amb.find('_');
                    if (us == std::string::npos) continue;
                    const ArcLedger::Key k{ sta, amb.substr(0, us), amb.substr(us + 1) };
                    arcReasonRecord(ctx.reasons, k, kArcReasonSlip,
                                    ctx.arc_ledger.find(k) != nullptr);
                }
            std::set<ArcLedger::Key> admitted;
            signalRosterAdmittedArcs(modeled, present, admitted);
            ctr.adm_rows += static_cast<long long>(admitted.size());
            std::vector<std::pair<ArcLedger::Key, ArcStep>> adm_edges;
            // The fold (phase 4a, S1).  When unarmed, the buffer is still passed and
            // admitEpoch sees `armed == false`, so every reason mask stays as
            // phase 3 wrote it.
            ctx.arc_ledger.admitEpoch(admitted, adm_edges, &ctx.reasons);
            for (const auto& [key, step] : adm_edges) {
                const int sta = std::get<0>(key);
                const std::string& sat = std::get<1>(key);
                const std::string& sig = std::get<2>(key);
                switch (step.transition) {
                case ArcTransition::Birth:   ++ctr.adm_birth;   break;
                case ArcTransition::Gap:     ++ctr.adm_gap;     break;
                case ArcTransition::Reentry: ++ctr.adm_reentry; break;
                case ArcTransition::Death:   ++ctr.adm_death;   break;
                default: break;
                }
                // The per-reason gap census (phase 4a, S1), read off the mask the
                // fold has just stamped.  A gap with two reasons increments both
                // bits, so the per-reason numbers do not sum to `gap_folded`;
                // `gap_multi` counts the gaps with more than one reason.
                if (ctx.reasons.armed && step.transition == ArcTransition::Gap) {
                    ++ctr.gap_folded;
                    int n_bits = 0;
                    for (int b = 0; b < kArcReasonCount; ++b)
                        if (step.reason_mask & (1u << b)) {
                            ++ctr.gap_by_reason[static_cast<std::size_t>(b)];
                            ++n_bits;
                        }
                    if (n_bits > 1) ++ctr.gap_multi;
                }
                // The carry-side census on the admitted lifecycle: returns at arcs
                // whose leveling carry is still live, the population card SID
                // measures, read off the admitted arc's own edge rather than the
                // carry's re-stash stamp.
                const bool carried =
                    ctx.carry.local_carry.count(std::make_tuple(sta, sat, sig)) > 0;
                if (step.transition == ArcTransition::Reentry && carried) {
                    ++ctr.carry_adm_gap_any;
                    if (step.gap_len_ep >= 2 && step.gap_len_ep <= 30) {
                        ++ctr.carry_adm_gap_2_30;
                        if (answered(sta, sat, sig)) ++ctr.carry_adm_gap_2_30_answered;
                    }
                }
                // The unified answered definition (phase 3, R4).  The census above
                // and phase 1 inferred the outcome (a read of `relevel.free_ambs`, a
                // match on `reentry_relevel` events), and the two inferences
                // disagree.  The response records what it did per arc, in both knob
                // states, and this census reads that record.  The split of carried
                // returns is four-way and exhaustive: an arc with no record was not
                // examined by any tier and is counted with BridgedNoVerdict.
                if (step.transition == ArcTransition::Reentry
                    && step.gap_len_ep >= 2 && step.gap_len_ep <= 30) {
                    ++ctr.ans_total;
                    if (!carried) {
                        ++ctr.ans_no_carry_entry;
                    } else {
                        const auto oit = ctx.carry.sid_reentry_outcome.find(key);
                        const ArcReentryOutcome o =
                            (oit == ctx.carry.sid_reentry_outcome.end())
                                ? ArcReentryOutcome::None : oit->second;
                        switch (o) {
                        case ArcReentryOutcome::Freed:            ++ctr.ans_freed;      break;
                        case ArcReentryOutcome::BridgedVerdict:   ++ctr.ans_bridged_v;  break;
                        case ArcReentryOutcome::NoCarry:          ++ctr.ans_no_carry;   break;
                        case ArcReentryOutcome::BridgedNoVerdict:
                        case ArcReentryOutcome::None:             ++ctr.ans_bridged_nv; break;
                        }
                        // The no-verdict population split by the reason mask of the
                        // gap that preceded the return (phase 4a, S1).  Returns that
                        // no raw tier saw are screen-side decouplings, and this split
                        // names the screen.  The mask is read off the arc rather than
                        // the step, because the step describes the re-entry while the
                        // mask was stamped at the gap and has survived it unchanged.
                        if (ctx.reasons.armed
                            && (o == ArcReentryOutcome::BridgedNoVerdict
                                || o == ArcReentryOutcome::None)) {
                            ++ctr.nv_total;
                            const ArcRecord* ar = ctx.arc_ledger.find(key);
                            const unsigned m = ar ? ar->adm_reason_mask : kArcReasonNone;
                            if (m == kArcReasonNone) ++ctr.nv_no_mask;
                            for (int b = 0; b < kArcReasonCount; ++b)
                                if (m & (1u << b))
                                    ++ctr.nv_by_reason[static_cast<std::size_t>(b)];
                        }
                    }
                }
                if (step.transition == ArcTransition::Reentry && step.state_recreated)
                    ++ctr.adm_state_recreated;
                const std::string& name =
                    (sta >= 0 && sta < static_cast<int>(rs.station_names.size()))
                        ? rs.station_names[static_cast<std::size_t>(sta)] : sat;
                const char* reason =
                    (step.reason_mask & kArcReasonHorizon) ? "horizon" : "";
                ++ctr.adm_trace_rows;
                traces.writeArcLedgerRow(se.time, name, sat, sig,
                                         arcStateName(step.from), arcStateName(step.to),
                                         step.generation, step.gap_len_ep, reason, "",
                                         "adm", arcStateName(step.adm_state),
                                         step.adm_gap_len_ep);
            }
            ctr.adm_live_max = std::max<long long>(
                ctr.adm_live_max,
                static_cast<long long>(ctx.arc_ledger.admittedLiveCount()));
        }

        // --- Phase 3 (R3b): the state-vector prune as a ledger mark -------------
        // Drained after the admitted pass, and the order matters: the prune ran in
        // PASS B of this epoch and removes the columns this epoch did not model, so
        // the arc a column belongs to enters its admitted Gap only when
        // `admitEpoch` above closes this epoch.  Drained before that advance, the
        // test `adm_state == Gap` is evaluated one epoch early and never fires.  No
        // behaviour change: the prune is unchanged and nothing here writes to the
        // solve.
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (!stations[i].sm) continue;
            for (const std::string& key : stations[i].sm->takePrunedAmbiguities()) {
                ++ctr.state_prune_keys;
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                const ArcLedger::Key ak{ static_cast<int>(i), parts->satellite,
                                         parts->signal };
                const ArcRecord* a = ctx.arc_ledger.find(ak);
                // Only a column removed while its admitted arc is in a Gap inside
                // the horizon is the silent death path the card names: past the
                // horizon the arc is dead anyway, and the ledger can say nothing
                // about a key it never admitted.
                if (a == nullptr || a->ep_last_admitted < 0) {
                    ++ctr.state_prune_unknown;
                    continue;
                }
                if (a->adm_state != ArcState::Gap) continue;
                if (rs.horizon_pl_s > 0.0
                    && (now_s - a->t_last_admitted_s) > rs.horizon_pl_s) continue;
                if (ctx.arc_ledger.markStatePruned(ak)) ++ctr.state_pruned_marks;
            }
        }


        // --- (a) the arc-level tracker's band signal against modelled_phase ----
        // Counted per row here; the (station, band) cell census is the run-end mode
        // of the same rows, the definition the SID audit's 46 cells use.
        if (raw != nullptr && raw->computed) {
            for (std::size_t i = 0; i < raw->rows.size() && i < rs.by_station.size(); ++i) {
                const StationRoster& sr = rs.by_station[i];
                for (const RawSlipObs& r : raw->rows[i]) {
                    for (const std::string* s : { &r.sig1, &r.sig2 }) {
                        if (s->size() < 2) continue;
                        const char band = (*s)[1];
                        std::string modelled;
                        for (const auto& [cons, cells] : sr) {
                            (void)cons;
                            const auto cit = cells.find(band);
                            if (cit != cells.end() && !cit->second.modelled_phase.empty()) {
                                modelled = cit->second.modelled_phase;
                                break;
                            }
                        }
                        ++rs.tracker_rows[std::make_tuple(static_cast<int>(i), band, *s)];
                        if (modelled.empty()) { ++ctr.trk_rows_no_modelled; continue; }
                        rs.modelled_of_cell[std::make_pair(static_cast<int>(i), band)] = modelled;
                        ++ctr.trk_rows;
                        if (*s != modelled) ++ctr.trk_rows_mismatch;
                    }
                }
            }
        }

        // --- (c) local_carry presence against the ledger, on both sides ------
        // The raw side is the phase-1 number and stays; the admitted side is the one
        // a carry reader keys on, and the difference between them is the structural
        // gap phase 2 addresses.
        for (const auto& [key, lc] : ctx.carry.local_carry) {
            (void)lc;
            ++ctr.lc_entries;
            const ArcLedger::Key ak{ std::get<0>(key), std::get<1>(key), std::get<2>(key) };
            const ArcRecord* a = ctx.arc_ledger.find(ak);
            if (a == nullptr) {
                ++ctr.lc_arc_unknown;
                ++ctr.lc_adm_arc_unknown;
                continue;
            }
            if (a->state == ArcState::Dead) ++ctr.lc_arc_dead;
            if (a->adm_state == ArcState::Dead)        ++ctr.lc_adm_arc_dead;
            else if (a->adm_state == ArcState::Absent) ++ctr.lc_adm_arc_unknown;
        }
        for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
            if (!a.modelled) continue;
            const bool carried = ctx.carry.local_carry.count(
                std::make_tuple(std::get<0>(key), std::get<1>(key), std::get<2>(key))) > 0;
            if (carried) continue;
            if (a.state == ArcState::Tracking)     ++ctr.lc_tracking_no_entry;
            if (a.adm_state == ArcState::Tracking) ++ctr.lc_adm_tracking_no_entry;
        }

        // --- (d) StateManager ambiguity presence against the ledger ------------
        // The prune is not touched: the comparison reads the surviving key set and
        // looks up each arc in the ledger, and separately counts the gapped arcs
        // inside the horizon that have lost their state.  The SID audit names that
        // second number the largest silent death path in the system.
        {
            // The ledger's key orders on the station index first, so each station's
            // arcs are one contiguous range.  Walking it that way keeps the class at
            // O(arcs) for the whole network instead of O(stations x arcs), which keeps
            // the observatory inside its cost bar at 305 stations.
            const auto& arcs = ctx.arc_ledger.arcs();
            for (std::size_t i = 0; i < stations.size(); ++i) {
                const StateManager* sm = stations[i].sm.get();
                if (sm == nullptr) continue;
                const int si = static_cast<int>(i);
                std::set<std::string> live;
                for (const auto& [k, idx] : sm->getAmbiguities()) {
                    (void)idx;
                    live.insert(k);
                    ++ctr.amb_states;
                    const auto us = k.find('_');
                    if (us == std::string::npos) continue;
                    const ArcLedger::Key ak{ si, k.substr(0, us), k.substr(us + 1) };
                    const ArcRecord* a = ctx.arc_ledger.find(ak);
                    if (a == nullptr) {
                        ++ctr.amb_arc_unknown;
                        ++ctr.amb_adm_arc_unknown;
                        continue;
                    }
                    if (a->state == ArcState::Gap) ++ctr.amb_arc_gapped;
                    if (a->adm_state == ArcState::Gap)         ++ctr.amb_adm_arc_gapped;
                    else if (a->adm_state == ArcState::Absent) ++ctr.amb_adm_arc_unknown;
                }
                for (auto it = arcs.lower_bound(ArcLedger::Key{ si, "", "" });
                     it != arcs.end() && std::get<0>(it->first) == si; ++it) {
                    if (!it->second.modelled) continue;
                    const bool alive =
                        live.count(std::get<1>(it->first) + "_" + std::get<2>(it->first)) > 0;
                    if (alive) continue;
                    if (it->second.state == ArcState::Gap)     ++ctr.amb_destroyed_in_gap;
                    if (it->second.adm_state == ArcState::Gap) ++ctr.amb_adm_destroyed_in_gap;
                }
            }
        }

        // --- (e) the anchor election's first sightings against the ledger ------
        // A key that has an incumbent now and had none at the previous epoch's read
        // is what the election treated as a first sighting.  The ledger is asked
        // whether its arcs had just returned, a distinction NetworkPhaseDatum.cpp
        // does not make.
        {
            std::set<std::string> now;
            for (const auto& [amb, sta] : ctx.carry.anchor_incumbent) {
                (void)sta;
                now.insert(amb);
                if (rs.anchor_prev.count(amb)) continue;
                ++ctr.anchor_first_sightings;
                const auto us = amb.find('_');
                if (us == std::string::npos) continue;
                const std::string sat = amb.substr(0, us);
                const std::string sig = amb.substr(us + 1);
                bool returned = false;
                for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
                    if (std::get<1>(key) != sat || std::get<2>(key) != sig) continue;
                    if (a.state == ArcState::Reentered
                        || (a.state == ArcState::Tracking && a.generation > 1
                            && a.ep_state_changed == ctx.arc_ledger.epochIndex())) {
                        returned = true;
                        break;
                    }
                }
                if (returned) ++ctr.anchor_first_sighting_reentry;
            }
            rs.anchor_prev.swap(now);
        }

        // --- (f) phase-bias family support per column-epoch, from the ledger ---
        // Carried support: an arc counts for its family only when it is a modelled
        // phase, alive in the ledger and present in local_carry.  The raw-side count
        // is a different quantity: the roster's arrival site is upstream of the
        // elevation mask and of every admission screen, so a satellite below the mask
        // is tracked and not carried, and the SID audit's medians are carried ones.
        {
            std::map<std::string, int> support;
            for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
                if (!a.modelled) continue;
                if (a.state != ArcState::Tracking && a.state != ArcState::Reentered) continue;
                if (!ctx.carry.local_carry.count(
                        std::make_tuple(std::get<0>(key), std::get<1>(key), std::get<2>(key))))
                    continue;
                ++support[familyKey(std::get<1>(key), std::get<2>(key))];
            }
            for (const auto& [fam, n] : support) rs.family_support[fam].push_back(n);
        }

        // --- (g) wire family coverage, from the emitter's own epoch ------------
        if (emitted) {
            // The families a consumer may request are the modelled signals of the
            // network, so the roster defines the denominator.  Built once per
            // epoch, not once per satellite.
            std::set<std::string> want_phase, want_code;
            for (std::size_t i = 0; i < rs.by_station.size(); ++i) {
                for (const auto& [cons, cells] : rs.by_station[i]) {
                    (void)cons;
                    for (const auto& [band, br] : cells) {
                        (void)band;
                        if (!br.modelled_phase.empty()) want_phase.insert(br.modelled_phase);
                        // The wire's coverage denominator is the set of families the
                        // network models, so a second phase belongs in it (card SID
                        // phase 6a).  Without it, the L1W family is never counted on a
                        // bed where no station latches L1W (the sim with a cloned
                        // second phase is such a bed), and `wire_pb_missing[L1W]`
                        // reads zero for the wrong reason.  Empty unless the
                        // second-phase channel or the all-channel mode is armed.
                        for (const auto& x : br.modelled_extra) want_phase.insert(x);
                        if (!br.modelled_code.empty())  want_code.insert(br.modelled_code);
                    }
                }
            }
            for (const auto& s : sol.sats) {
                if (!s.has_clock) continue;
                ++ctr.wire_sat_epochs;
                for (const std::string& sig : want_phase) {
                    if (s.phase_bias.count(sig)) ++ctr.wire_pb_present[sig];
                    else                         ++ctr.wire_pb_missing[sig];
                }
                for (const std::string& sig : want_code)
                    if (!s.code_bias_m.count(sig)) ++ctr.wire_cb_missing[sig];
            }
        }

        // --- The answered half, and the carry-side gap census ------------------
        // The `answered` test is defined at the top of this pass (the admitted census
        // uses it too).
        (void)freed;
        for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
            if (a.state != ArcState::Reentered) continue;
            if (a.gap_len_ep < 2 || a.gap_len_ep > 30) continue;
            if (!ctx.carry.local_carry.count(
                    std::make_tuple(std::get<0>(key), std::get<1>(key), std::get<2>(key))))
                continue;
            if (answered(std::get<0>(key), std::get<1>(key), std::get<2>(key)))
                ++ctr.gap_2_30_answered;
        }

        // The carry-side census, driven epoch to epoch by the carry's own re-stash
        // stamp.  This is the SID audit's population: `network_amb_state.csv` writes
        // a row when the harvest stashes an arc, and an entry that stays in the map
        // without being re-stashed leaves no row, so map presence would report almost
        // no gaps.  The stamp is @c LocalCarry::last_seen_s, set at every stash.
        // Reported separately from the raw-side population above, never added to it.
        {
            const long long ep = ctx.arc_ledger.epochIndex();
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            for (const auto& [key, lc] : ctx.carry.local_carry) {
                if (std::abs(lc.last_seen_s - now_s) > 1.0) continue;  // not stashed here
                ++ctr.carry_entries_seen;
                auto it = rs.carry_last_ep.find(key);
                if (it != rs.carry_last_ep.end()) {
                    const long long gap = ep - it->second - 1;
                    if (gap >= 1) {
                        ++ctr.carry_gap_any;
                        if (gap >= 2 && gap <= 30) {
                            ++ctr.carry_gap_2_30;
                            if (answered(std::get<0>(key), std::get<1>(key),
                                         std::get<2>(key)))
                                ++ctr.carry_gap_2_30_answered;
                        }
                    }
                    it->second = ep;
                } else {
                    rs.carry_last_ep.emplace(key, ep);
                }
            }
            // Bound the memory: an entry last seen more than 60 epochs ago can no
            // longer produce a 2-to-30 epoch gap, and keeping it would only turn a
            // long return into a census row this class does not count.
            for (auto it = rs.carry_last_ep.begin(); it != rs.carry_last_ep.end(); ) {
                if (ep - it->second > 60) it = rs.carry_last_ep.erase(it);
                else ++it;
            }
        }
    }

    // =====================================================================
    // The gate-drop edge (class h)
    // =====================================================================

    void networkSignalRosterGateDrop(NetworkRunContext& ctx, const std::string& sat,
                                     const std::string& signal) {
        if (!ctx.roster.armed) return;
        auto& ctr = ctx.counters.sid;
        ++ctr.gate_drop_edges;
        bool gapped = false, tracking = false;
        for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
            if (std::get<1>(key) != sat || std::get<2>(key) != signal) continue;
            if (a.state == ArcState::Gap) gapped = true;
            if (a.state == ArcState::Tracking || a.state == ArcState::Reentered)
                tracking = true;
        }
        if (gapped)             ++ctr.gate_drop_edges_gapped;
        else if (tracking)      ++ctr.gate_drop_edges_tracking;
        // The same edges, read on the admitted side and per arc (phase 4a, S2).
        // Class (h) above records whether the family holds a gapped arc on the raw
        // side; this counts how many of its arcs are alive, gapped or dead, which is
        // what the generation test at the family's return is decided against.
        if (!ctx.reasons.armed) return;
        ++ctr.anchor_drop_edges;
        int n_tr = 0, n_gp = 0, n_dd = 0;
        anchorReentryFamilyCensus(ctx.arc_ledger, sat, signal, n_tr, n_gp, n_dd);
        ctr.anchor_drop_arcs_tracking += n_tr;
        ctr.anchor_drop_arcs_gap      += n_gp;
        ctr.anchor_drop_arcs_dead     += n_dd;
    }

    // =====================================================================
    // The anchor election's ledger read (S2) and its restore decisions (B1)
    // =====================================================================

    void networkSignalRosterAnchorRead(NetworkRunContext& ctx,
                                       NetworkRunTraces& traces,
                                       const GnssTime& t,
                                       const AnchorReentry& re) {
        if (!re.observe || !ctx.roster.armed) return;
        auto& ctr = ctx.counters.sid;
        ctr.anchor_fs        += re.n_first_sighting;
        ctr.anchor_fs_return += re.n_first_sighting_return;
        ctr.anchor_restore         += re.n_restore;
        ctr.anchor_restore_refused += re.n_restore_refused;
        ctr.anchor_reentry_new     += re.n_reentry_new;
        for (const AnchorFirstSighting& fs : re.first_sightings) {
            if (fs.cls == AnchorReentryClass::SameGeneration) ++ctr.anchor_fs_same_gen;
            else if (fs.cls == AnchorReentryClass::NewGeneration) ++ctr.anchor_fs_new_gen;
            // The identity the SID audit reports this class as lacking: which family,
            // how long it had been without an incumbent, how many of its arcs had
            // just come back, and how the generation test classified it.  Throttled
            // on the ledger's per (station, transition) holdoff, here under station
            // index -1, so the stream stays bounded at 305 stations; the counters
            // carry the full volume.
            const bool interesting = fs.n_returning > 0
                                     || fs.cls != AnchorReentryClass::NoMemory;
            if (!interesting) continue;
            const std::size_t us = fs.amb.find('_');
            const std::string sat = us == std::string::npos ? fs.amb : fs.amb.substr(0, us);
            const std::string sig = us == std::string::npos ? std::string{}
                                                            : fs.amb.substr(us + 1);
            if (ctx.arc_ledger.eventAllowed(-1, ArcTransition::Reentry)) {
                ++ctr.anchor_fs_events;
                traces.emitEvent(t, "ARC", "anchor_first_sighting_on_return", sat,
                                 fs.station,
                                 static_cast<double>(fs.n_returning),
                                 fs.since_incumbent_ep < 0
                                     ? std::nullopt
                                     : std::optional<double>(
                                           static_cast<double>(fs.since_incumbent_ep)),
                                 fs.restored ? "restore" : "elect",
                                 fmt::format("signal={};since_incumbent_ep={};"
                                             "returning={};tracking={};class={};"
                                             "prev_anchor={};elected={};refused={}",
                                             sig, fs.since_incumbent_ep,
                                             fs.n_returning, fs.n_tracking,
                                             anchorReentryClassName(fs.cls),
                                             fs.prev_anchor.empty() ? std::string("-")
                                                                    : fs.prev_anchor,
                                             fs.station,
                                             fs.refused[0] == '\0' ? "-" : fs.refused));
            }
            // The two edges of the restore behaviour (B1), emitted only when the
            // behaviour half is armed, so an observatory run cannot be mistaken for
            // one that acted.
            if (!re.restore || fs.cls == AnchorReentryClass::NoMemory) continue;
            traces.emitEvent(t, "PHASE_ANCHOR",
                             fs.restored ? "reentry_restore" : "reentry_new", sat,
                             fs.restored ? fs.prev_anchor : fs.station,
                             static_cast<double>(fs.n_returning),
                             std::nullopt,
                             fs.restored ? "restore" : "elect",
                             fmt::format("signal={};class={};prev_anchor={};elected={};"
                                         "since_incumbent_ep={};refused={}",
                                         sig, anchorReentryClassName(fs.cls),
                                         fs.prev_anchor.empty() ? std::string("-")
                                                                : fs.prev_anchor,
                                         fs.station, fs.since_incumbent_ep,
                                         fs.refused[0] == '\0' ? "-" : fs.refused));
        }
    }

    // =====================================================================
    // The run-end table
    // =====================================================================

    void networkSignalRosterRunEnd(NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;
        auto& c = ctx.counters.sid;

        // Class (a), the cell census: per (station, band), the dominant tracker
        // signal over the run against that cell's modelled phase.  That is the SID
        // audit's definition of a mismatched station-band cell.
        std::map<std::pair<int, char>, std::pair<std::string, long long>> mode;
        for (const auto& [k, n] : rs.tracker_rows) {
            const auto cell = std::make_pair(std::get<0>(k), std::get<1>(k));
            auto& best = mode[cell];
            if (n > best.second) best = { std::get<2>(k), n };
        }
        std::set<int> mismatched_stations;
        for (const auto& [cell, best] : mode) {
            const auto mit = rs.modelled_of_cell.find(cell);
            if (mit == rs.modelled_of_cell.end() || mit->second.empty()) continue;
            ++c.trk_cells;
            if (best.first == mit->second) continue;
            ++c.trk_cells_mismatch;
            if (cell.second == '1')      ++c.trk_cells_b1;
            else if (cell.second == '2') ++c.trk_cells_b2;
            mismatched_stations.insert(cell.first);
        }
        c.trk_stations_mismatch = static_cast<long long>(mismatched_stations.size());

        // Class (b).
        c.rule_stations = static_cast<long long>(rs.rule_reached.size());
        c.rule_stations_differ = static_cast<long long>(rs.rule_differs.size());
        c.rule_stations_differ_b1 = static_cast<long long>(rs.rule_differs_b1.size());
        c.rule_unreachable = static_cast<long long>(rs.station_names.size()
                                                    - rs.rule_reached.size());

        // Class (f): the median support per family, and the thin population.
        std::map<std::string, std::vector<double>> med_by_signal;
        for (const auto& [fam, v] : rs.family_support) {
            if (v.empty()) continue;
            ++c.fam_families;
            const std::size_t thin =
                static_cast<std::size_t>(std::count_if(v.begin(), v.end(),
                                                       [](int n) { return n < 3; }));
            if (static_cast<double>(thin) > 0.10 * static_cast<double>(v.size())) ++c.fam_thin;
            const auto bar = fam.find('|');
            const std::string sig = (bar == std::string::npos) ? fam : fam.substr(bar + 1);
            med_by_signal[sig].push_back(medianOf(v));
        }
        std::string fam_line;
        for (auto& [sig, meds] : med_by_signal) {
            std::sort(meds.begin(), meds.end());
            const std::size_t n = meds.size();
            const double med = n ? ((n % 2) ? meds[n / 2]
                                            : 0.5 * (meds[n / 2 - 1] + meds[n / 2]))
                                 : 0.0;
            if (!fam_line.empty()) fam_line += ", ";
            fam_line += fmt::format("{} {:.1f} over {} famil{}", sig, med, n,
                                    n == 1 ? "y" : "ies");
        }

        std::string wire_line;
        for (const auto& [sig, n] : c.wire_pb_missing) {
            const auto p = c.wire_pb_present.find(sig);
            const long long have = (p == c.wire_pb_present.end()) ? 0 : p->second;
            if (!wire_line.empty()) wire_line += ", ";
            wire_line += fmt::format("{} {} of {} missing", sig, n, n + have);
        }
        if (wire_line.empty()) wire_line = "none";
        std::string wire_cb;
        for (const auto& [sig, n] : c.wire_cb_missing) {
            if (!wire_cb.empty()) wire_cb += ", ";
            wire_cb += fmt::format("{} {}", sig, n);
        }
        if (wire_cb.empty()) wire_cb = "none";

        log->info(
            "Network (card SID phase 1 / SIGNAL ROSTER + ARC LEDGER, OBSERVATORY): "
            "the run-end table.  BUILD: {} epoch(s), {} (station, constellation, "
            "band) cell(s) of which {} carry a modelled role, {} tracked signal(s), "
            "{} companion(s), {} exclusion(s) [{} allowlist + {} not_latched + {} "
            "no_latch].  P2 SELF-CHECK roster_latch_disagree = {} (bar 0: the roster "
            "is BUILT from the latch, so any other value is a defect in "
            "SignalRoster.cpp, not a finding about this run).{}",
            c.epochs, c.cells, c.cells_modelled, c.tracked_signals,
            c.companion_signals, c.excluded_signals, c.excl_allowlist,
            c.excl_not_latched, c.excl_no_latch, c.roster_latch_disagree,
            c.epochs == 0
                ? "  ⚠ ARMED AND INERT: the build seam never ran; this is dead "
                  "wiring, not a quiet run."
                : "");

        log->info(
            "Network (card SID phase 1 / DISAGREEMENT CLASSES): "
            "(a) TRACKER vs modelled_phase: {} of {} compared row-slot(s) name a "
            "different signal ({} slot(s) had no modelled phase to compare against); "
            "run-end CELL census {} of {} (station, band) cell(s) whose DOMINANT "
            "tracker signal is not the modelled one, at {} station(s) [band 1 {}, "
            "band 2 {}].  "
            "(b) COUNT rule vs PRIORITY rule: {} of {} station(s) latch differently on "
            "at least one (band, obs type), of which {} differ on the BAND-1 PHASE "
            "(card CSL's own population); {} station(s) unreachable: their candidate "
            "list was never retained.  "
            "(c) local_carry vs the ledger: {} entr(y|ies) examined, {} at a DEAD arc, "
            "{} at an arc the ledger never saw, and {} MODELLED arc(s) Tracking with "
            "no carry entry.  "
            "(d) StateManager ambiguities vs the ledger: {} state(s) examined, {} "
            "alive while their arc is in a GAP, {} with no arc at all, and {} "
            "MODELLED arc(s) in a GAP inside the horizon whose state is already gone "
            "(the silent death path).  "
            "(e) ANCHOR election vs the ledger: {} first sighting(s), of which {} are "
            "a family whose arcs had just RETURNED.  "
            "(f) PHASE-BIAS family support per column-epoch, median by signal: {}; "
            "{} of {} famil(y|ies) sit below 3 carried stations in more than 10 per "
            "cent of their alive epochs.  "
            "(g) WIRE family coverage over {} emitted satellite-epoch(s) with a clock "
            "record: phase bias {}; code bias {}.  "
            "(h) COVERAGE-GATE drop edges: {} seen, {} on a family with at least one "
            "arc in a GAP, {} on a family that is entirely tracking.",
            c.trk_rows_mismatch, c.trk_rows, c.trk_rows_no_modelled,
            c.trk_cells_mismatch, c.trk_cells, c.trk_stations_mismatch,
            c.trk_cells_b1, c.trk_cells_b2,
            c.rule_stations_differ, c.rule_stations, c.rule_stations_differ_b1,
            c.rule_unreachable,
            c.lc_entries, c.lc_arc_dead, c.lc_arc_unknown, c.lc_tracking_no_entry,
            c.amb_states, c.amb_arc_gapped, c.amb_arc_unknown, c.amb_destroyed_in_gap,
            c.anchor_first_sightings, c.anchor_first_sighting_reentry,
            fam_line.empty() ? std::string("none") : fam_line, c.fam_thin, c.fam_families,
            c.wire_sat_epochs, wire_line, wire_cb,
            c.gate_drop_edges, c.gate_drop_edges_gapped, c.gate_drop_edges_tracking);

        log->info(
            "Network (card SID phase 1 / ARC LEDGER): horizon {:.0f} s [= max(pl {:.0f}, "
            "iono {:.0f}, pb_hold {:.0f}, readmit {:.0f})], forget {} epoch(s).  "
            "TRANSITIONS: {} birth, {} gap, {} reentry, {} death; {} re-entry(ies) "
            "after any gap, {} after 2 to 30 missed epochs at an arc with a live "
            "carry, of which {} were ANSWERED by the gap re-entry tier at the return "
            "epoch and {} were NOT.  ⚠ THAT POPULATION IS RAW-SIDE: the roster's "
            "arrival site is upstream of the elevation mask, the allow list and every "
            "admission screen, so the ledger tracks what the RECEIVER reported, which "
            "is a strict superset of what the estimator carries.  The CARRY-SIDE "
            "census, which is the population the card's 195 / 68 were measured on "
            "(the carry's RE-STASH stamp epoch to epoch, which is exactly what "
            "network_amb_state.csv writes a row for), reads {} return(s) after at "
            "least one missed epoch over {} re-stash(es) walked, {} of them after 2 to "
            "30 missed epochs, {} ANSWERED and {} NOT.  Peak {} live arc(s), {} "
            "retained record(s).  {} trace row(s) in "
            "network_arc_ledger.csv (one per transition), {} ARC event row(s) in "
            "network_events.csv (holdoff {} epoch(s) per station and transition, "
            "births suppressed on the first epoch).  COST: build {:.1f} ms total "
            "({:.2f} ms/epoch), audit {:.1f} ms total ({:.2f} ms/epoch).{}",
            rs.horizon_s, rs.horizon_pl_s, rs.horizon_iono_s, rs.horizon_pbhold_s,
            rs.horizon_readmit_s, kArcLedgerForgetEp,
            c.arc_birth, c.arc_gap, c.arc_reentry, c.arc_death,
            c.gap_any, c.gap_2_30, c.gap_2_30_answered,
            c.gap_2_30 - c.gap_2_30_answered,
            c.carry_gap_any, c.carry_entries_seen, c.carry_gap_2_30,
            c.carry_gap_2_30_answered, c.carry_gap_2_30 - c.carry_gap_2_30_answered,
            c.arc_live_max, c.arc_known_max, c.arc_trace_rows, c.arc_events,
            kArcLedgerEventHoldoffEp,
            c.build_ms, c.epochs ? c.build_ms / static_cast<double>(c.epochs) : 0.0,
            c.audit_ms, c.epochs ? c.audit_ms / static_cast<double>(c.epochs) : 0.0,
            c.arc_birth == 0
                ? "  ⚠ ARMED AND INERT: ZERO arcs were ever born, so the ledger saw "
                  "no observation at all.  This is dead wiring, not a quiet run."
                : "");

        // ---------------------------------------------------------------
        // Phase 2: the reader, the writer and the admitted lifecycle
        // ---------------------------------------------------------------
        std::string datum_line;
        for (const auto& [k, n] : c.datum_code_census) {
            if (!datum_line.empty()) datum_line += ", ";
            datum_line += fmt::format("{} ({} cell-epochs)", k, n);
        }
        if (datum_line.empty()) datum_line = "none";
        std::string datum_ph_line;
        for (const auto& [k, n] : c.datum_phase_census) {
            if (!datum_ph_line.empty()) datum_ph_line += ", ";
            datum_ph_line += fmt::format("{} ({} cell-epochs)", k, n);
        }
        if (datum_ph_line.empty()) datum_ph_line = "none";

        log->info(
            "Network (card SID phase 2 / THE READER, THE WRITER AND THE ADMITTED "
            "LIFECYCLE): "
            "TRACKER READ (network.roster_tracker): {} lookup(s) answered by a roster "
            "cell, {} fell back to the station LATCH (the roster is built upstream of "
            "computeModels, so the FIRST epoch of a constellation has no modelled role "
            "yet; a count far above one epoch's worth of cells means the reader is not "
            "reading the roster), {} answered by neither (a band with no latch at all).  "
            "IF PIN AS A WRITER: {} epoch(s) with a realized pin, {} per-SATELLITE pin(s) "
            "walked, {} roster cell(s) given a datum CODE of which {} also carry a datum "
            "PHASE and {} could not (the pinned code is not that station's modelled "
            "code); {} cell(s) sat on a (constellation, band) whose satellites pinned "
            "MORE THAN ONE code, where the designation written is the mode.  Datum code "
            "by (constellation, band): {}.  Datum phase: {}.  "
            "ADMITTED LIFECYCLE: {} admitted phase row(s) fed over the run, peak {} "
            "admitted-live arc(s); TRANSITIONS {} birth, {} gap, {} reentry, {} death "
            "({} trace row(s), side=adm).  "
            "(c) local_carry, ADMITTED side: {} of {} entr(y|ies) at a DEAD admitted arc, "
            "{} at an arc never admitted, {} MODELLED arc(s) admitted-Tracking with no "
            "carry entry [RAW side, for comparison: {} dead, {} unknown, {} tracking "
            "without an entry].  "
            "(d) StateManager ambiguities, ADMITTED side: {} of {} state(s) alive while "
            "their ADMITTED arc is in a GAP, {} with no admitted arc, and {} MODELLED "
            "arc(s) in an ADMITTED GAP inside the horizon whose state is already gone "
            "[RAW side: {} gapped, {} unknown, {} destroyed in a gap].  "
            "CARRY-SIDE GAP CENSUS on the ADMITTED lifecycle: {} return(s) at an arc "
            "with a live carry, {} of them after 2 to 30 missed epochs, {} ANSWERED and "
            "{} NOT [the phase-1 RE-STASH-STAMP reading of the same population, kept for "
            "comparison: {} / {} / {} / {}].{}",
            c.trk_roster_hits, c.trk_roster_fallback, c.trk_roster_none,
            c.datum_epochs, c.datum_sat_pins, c.datum_cells, c.datum_cells_phase,
            c.datum_cells_off_model, c.datum_cells_ambiguous,
            datum_line, datum_ph_line,
            c.adm_rows, c.adm_live_max,
            c.adm_birth, c.adm_gap, c.adm_reentry, c.adm_death, c.adm_trace_rows,
            c.lc_adm_arc_dead, c.lc_entries, c.lc_adm_arc_unknown,
            c.lc_adm_tracking_no_entry,
            c.lc_arc_dead, c.lc_arc_unknown, c.lc_tracking_no_entry,
            c.amb_adm_arc_gapped, c.amb_states, c.amb_adm_arc_unknown,
            c.amb_adm_destroyed_in_gap,
            c.amb_arc_gapped, c.amb_arc_unknown, c.amb_destroyed_in_gap,
            c.carry_adm_gap_any, c.carry_adm_gap_2_30,
            c.carry_adm_gap_2_30_answered,
            c.carry_adm_gap_2_30 - c.carry_adm_gap_2_30_answered,
            c.carry_gap_any, c.carry_gap_2_30, c.carry_gap_2_30_answered,
            c.carry_gap_2_30 - c.carry_gap_2_30_answered,
            c.adm_rows == 0
                ? "  ⚠ ARMED AND INERT: ZERO admitted rows were ever fed, so the "
                  "admitted lifecycle never advanced.  This is dead wiring, not a "
                  "quiet run."
                : "");

        // ---------------------------------------------------------------
        // Phase 3: the prune and expiry edges, and the unified answered split
        // ---------------------------------------------------------------
        log->info(
            "Network (card SID phase 3 / PRUNE AND EXPIRY EDGES): "
            "(R3a) the leveling carry's gap prune retired {} entr(y|ies) at "
            "phase_leveling_max_gap_s = {:.0f} s, each now an ARC arc_death with "
            "reason=horizon ({} evented, holdoff {} epoch(s) per station and "
            "transition); that edge emitted NOTHING before this phase.  "
            "(R3b) the state-vector prune destroyed {} ambiguity column(s) over the "
            "run, of which {} were MARKED state_pruned because their ADMITTED arc "
            "was in a GAP inside the carry horizon (the silent death path of the "
            "audit, now on the record) and {} belonged to an arc the ledger has "
            "never admitted; {} admitted re-entry(ies) then landed on a RE-CREATED "
            "column.  The prune itself is unchanged: no behaviour moved here.{}",
            c.carry_prune_deaths, rs.horizon_pl_s, c.carry_prune_events,
            kArcLedgerEventHoldoffEp,
            c.state_prune_keys, c.state_pruned_marks, c.state_prune_unknown,
            c.adm_state_recreated,
            (c.state_prune_keys > 0 && c.state_pruned_marks == 0)
                ? "  ⚠ Every pruned column belonged to an arc that was NOT in an "
                  "admitted gap inside the horizon: read the two denominators "
                  "before concluding the path is quiet."
                : "");

        const auto pct = [](long long n, long long d) {
            return d > 0 ? 100.0 * static_cast<double>(n) / static_cast<double>(d) : 0.0;
        };
        log->info(
            "Network (card SID phase 3 / THE UNIFIED ANSWERED SPLIT, R4): over "
            "ADMITTED re-entries after 2 to 30 missed epochs, read off the "
            "RESPONSE's OWN record per arc rather than inferred: {} in the "
            "population, {} FREED ({:.1f} per cent), {} BRIDGED BY VERDICT (a raw "
            "tier saw the return and called it clean, {:.1f} per cent), {} BRIDGED "
            "WITH NO VERDICT (nothing looked at the return at all, {:.1f} per "
            "cent), {} reached with NO CARRY to protect, {} with no local_carry "
            "entry at all.  This REPLACES the two approximations of phases 1 and 2 "
            "(the relevel.free_ambs read and the reentry_relevel event match), "
            "which disagreed by 5 of 51; both are still printed above for "
            "comparison.  LEDGER-DRIVEN RESPONSE (network.ledger_reentry) is {}.{}",
            c.ans_total, c.ans_freed, pct(c.ans_freed, c.ans_total),
            c.ans_bridged_v, pct(c.ans_bridged_v, c.ans_total),
            c.ans_bridged_nv, pct(c.ans_bridged_nv, c.ans_total),
            c.ans_no_carry, c.ans_no_carry_entry,
            ctx.knobs.ledger_reentry ? "ON" : "OFF",
            c.ans_total == 0
                ? "  ⚠ EMPTY POPULATION: no admitted arc returned after 2 to 30 "
                  "missed epochs in this run, so the split says nothing about the "
                  "response.  Read it as a denominator, not as a clean result."
                : "");

        // ---------------------------------------------------------------
        // Phase 4a: the reason-mask writers (S1)
        // ---------------------------------------------------------------
        if (!ctx.reasons.armed) return;
        {
            std::string per_reason, per_writer, nv_split;
            for (int b = 0; b < kArcReasonCount; ++b) {
                const unsigned bit = 1u << b;
                const std::size_t ub = static_cast<std::size_t>(b);
                if (!per_reason.empty()) per_reason += ", ";
                per_reason += fmt::format("{} {}", arcReasonName(bit), c.gap_by_reason[ub]);
                const long long calls = ctx.reasons.calls[ub];
                const long long unk   = ctx.reasons.calls_unknown[ub];
                if (calls > 0 || unk > 0) {
                    if (!per_writer.empty()) per_writer += ", ";
                    per_writer += fmt::format("{} {}{}", arcReasonName(bit), calls,
                                              unk > 0 ? fmt::format(" ({} reached no arc)", unk)
                                                      : std::string{});
                }
                if (c.nv_by_reason[ub] > 0) {
                    if (!nv_split.empty()) nv_split += ", ";
                    nv_split += fmt::format("{} {}", arcReasonName(bit), c.nv_by_reason[ub]);
                }
            }
            // The writers that never fired, named rather than left to be inferred
            // from a zero in a list (standing invariant 4: a zero with a zero
            // denominator is dead wiring and the teardown says so).
            std::string silent;
            for (int b = 0; b < kArcReasonCount; ++b) {
                const unsigned bit = 1u << b;
                if (bit == kArcReasonUnknown) continue;   // not a writer
                if (ctx.reasons.calls[static_cast<std::size_t>(b)] > 0) continue;
                if (!silent.empty()) silent += ", ";
                silent += arcReasonName(bit);
            }
            log->info(
                "Network (card SID phase 4a / REASON-MASK WRITERS, S1): {} admitted "
                "GAP(s) were stamped with a reason, {} of them carrying more than one "
                "(so the per-reason counts below do not sum to the total).  GAPS "
                "OPENED PER REASON: {}.  WRITER CALLS (arc records made, per reason): "
                "{}.  SILENT WRITERS (armed and never fired): {}.  A gross-LOS "
                "DE-WEIGHT is recorded as 'deweight' and is EXCLUDED from every gap "
                "mask by construction, because the row still folds.  'unknown' is an "
                "admitted gap that no writer explains.{}",
                c.gap_folded, c.gap_multi, per_reason,
                per_writer.empty() ? "none" : per_writer,
                silent.empty() ? "none" : silent,
                c.gap_folded == 0
                    ? "  ⚠ ARMED AND INERT: no admitted gap was folded at all, so the "
                      "reason masks say nothing about this run."
                    : "");
            // This is not the phase-3 `ledger_reentry_no_verdict` population: that one
            // is every admitted return inside the carry horizon and is attributed by
            // the response's own line in NetworkTeardown.cpp.  This is the
            // bridged-with-no-verdict bucket of the R4 split, whose population is
            // narrowed to returns after 2 to 30 missed epochs.  The two are logged
            // separately; a single "no-verdict by reason" number would be one or the
            // other without saying which.
            log->info(
                "Network (card SID phase 4a / THE R4 BRIDGED-NO-VERDICT BUCKET BY "
                "REASON, S1): {} re-entr(y|ies) of the 2-to-30-epoch R4 population that "
                "no raw tier saw, split by the reason mask of the GAP THAT PRECEDED the "
                "return: {}.  {} of them carry NO mask at all (the gap opened before the "
                "writers were armed, or outside the fold).  The card's own no-verdict "
                "population (the response's, over the whole carry horizon) is "
                "attributed on the phase-3 line above.{}",
                c.nv_total, nv_split.empty() ? "none" : nv_split, c.nv_no_mask,
                c.nv_total == 0
                    ? "  ⚠ EMPTY POPULATION: no no-verdict re-entry occurred, so the "
                      "split is a denominator and not a result."
                    : "");
        }

        // ---------------------------------------------------------------
        // Phase 4a / 4b: the anchor election read against the ledger (S2 / B1 / B2)
        // ---------------------------------------------------------------
        log->info(
            "Network (card SID phase 4 / THE ANCHOR ELECTION AGAINST THE LEDGER, S2): "
            "the election treated {} (satellite, signal) famil(y|ies) as a FIRST "
            "SIGHTING; {} of them ({:.1f} per cent) had at least one arc the ledger "
            "calls Reentered or Gap, which is a RETURN read as a birth — the class the "
            "audit named at `anchor_incumbent`.  Of those, {} were a SAME-GENERATION "
            "return of a family a coverage-gate DROP EDGE had erased and {} came back "
            "as NEW generations; {} identity event(s) written (holdoff {} epoch(s)).  "
            "COVERAGE-GATE DROP EDGES seen by this class: {}, carrying {} arc(s) "
            "Tracking, {} Gap or Reentered and {} Dead at the edge.  ANCHOR_ARC_AGE "
            "against the ledger's admitted age: {} compared, {} DISAGREE, {} arc(s) "
            "the ledger could give no admitted age for.{}",
            c.anchor_fs, c.anchor_fs_return, pct(c.anchor_fs_return, c.anchor_fs),
            c.anchor_fs_same_gen, c.anchor_fs_new_gen, c.anchor_fs_events,
            kArcLedgerEventHoldoffEp,
            c.anchor_drop_edges, c.anchor_drop_arcs_tracking,
            c.anchor_drop_arcs_gap, c.anchor_drop_arcs_dead,
            c.age_compared, c.age_disagree, c.age_no_ledger,
            c.age_compared == 0
                ? "  ⚠ ZERO DENOMINATOR on the age class: `anchor_arc_age` exists only "
                  "while network.eq34_anchor_min_arc_ep, the pin bridge or the "
                  "fresh-pin ramp is armed, and none of them is, so the hygiene seam "
                  "never ran.  This is DEAD WIRING, not an agreement."
                : "");
        log->info(
            "Network (card SID phase 4b / THE RESTORE, B1 and B2): network.ledger_anchor "
            "is {}.  {} return(s) RESTORED the previous incumbent, {} same-generation "
            "return(s) were REFUSED by the election's own rules (untracked, banned, "
            "hygiene or basis), and {} return(s) proceeded as today because their arcs "
            "were NEW generations.  {} arc age(s) were taken from the ledger instead of "
            "the counter.{}",
            ctx.knobs.ledger_anchor ? "ON" : "OFF",
            c.anchor_restore, c.anchor_restore_refused, c.anchor_reentry_new,
            c.age_from_ledger,
            (c.anchor_restore + c.anchor_restore_refused + c.anchor_reentry_new) == 0
                ? "  ⚠ EMPTY POPULATION: no family that a drop edge erased returned in "
                  "this run, so the restore changed nothing here."
                : "");
    }

} // namespace GnssProcessor
