/**
 * @file SignalRoster.cpp
 * @brief Card SID phase 1: the signal roster and its observatory (see SignalRoster.hpp).
 */
#include "app/SignalRoster.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"        // NetStation (roster + its PppModel / StateManager)
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
            const std::set<std::string>* allowlist) {
        std::map<char, BandRoster> out;

        // 1. The TRACKED sets, per band digit, deduplicated and ascending.
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

        // 2. The MODELLED roles, REPRODUCED from the latch and never re-derived.
        //    A band the latch names but the receiver does not report this epoch
        //    still gets its cell: the latch is the estimator's key, and a reader
        //    asking "what does this station carry on band 1" must get an answer on
        //    the epochs the arc is absent too.
        for (const RosterLatchRow& r : latch) {
            if (r.signal.size() < 2) continue;
            const char band = r.signal[1];
            if (r.obs_type == 'L')      out[band].modelled_phase = r.signal;
            else if (r.obs_type == 'C') out[band].modelled_code  = r.signal;
        }

        // 3. The COMPANION codes (card SCC).  Band 1 only, the allow list walked in
        //    its configured order, the canonical skipped, the untracked skipped and
        //    duplicates skipped, with the canonical required PRESENT this epoch,
        //    which is the rule NetworkSecondCodeScope.cpp and PppModel implement.
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

        // 4. The EXCLUDED signals, with the reason that actually applies.  The allow
        //    list is the earlier gate, so it names the exclusion where it bites; a
        //    band the latch carries names the signal it chose instead; a band with
        //    no latch at all is a different statement and gets its own reason.
        for (auto& [band, br] : out) {
            (void)band;
            const auto note = [&](const std::string& sig, const std::string& modelled,
                                  bool companion) {
                if (sig == modelled) return;
                if (companion) return;
                const char* reason = "no_latch";
                if (allowlist && !allowlist->empty() && !allowlist->count(sig))
                    reason = "allowlist";
                else if (!modelled.empty())
                    reason = "not_latched";
                br.excluded.emplace_back(sig, reason);
            };
            for (const std::string& s : br.tracked_phase) note(s, br.modelled_phase, false);
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
        // then takes the LEXICALLY LOWEST such phase of the band, which is what the
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

        const auto& nc = config_.getNetworkConfig();
        const bool companions_armed = config_.networkSecondCodeChannel();
        const std::vector<std::string>& companion_allow = config_.networkSecondCodeSignals();
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
            // PRIORITY LIST is skipped outright: the latch's candidate pool is that
            // list, so such a constellation "silently produces no canonicals" and the
            // roster would describe signals the producer never models.  The RINEX
            // files carry every constellation the receiver tracked, so without this
            // the roster would build cells (and arcs) for GLONASS, Galileo and BeiDou
            // on a GPS-only posture, which is both wrong and the dominant cost.
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
                // The latch is decided ONCE per constellation and never revised, so
                // the cache is refilled only while it is still empty.  The first
                // epoch of a constellation is decided INSIDE computeModels, which is
                // downstream of this seam, so that epoch has no latch and the cells
                // carry Tracked roles alone.
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
                                                    companions_armed, allow);

                // P2's SELF-CHECK.  The roster is built FROM the latch, so a nonzero
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
                        (void)sig;
                        ++ctr.excluded_signals;
                        const std::string r = reason;
                        if (r == "allowlist")        ++ctr.excl_allowlist;
                        else if (r == "not_latched") ++ctr.excl_not_latched;
                        else                         ++ctr.excl_no_latch;
                    }
                }

                // Class (b): the count rule against the priority rule, run ONCE per
                // (station, constellation) as soon as the candidate list exists.
                // A constellation with no candidate list is simply not comparable yet
                // (the latch is decided INSIDE computeModels, downstream of this
                // seam), and the roster sees every constellation in the RAW record,
                // including ones this run does not process at all.  Reachability is
                // therefore tracked POSITIVELY, per station: a station counts as
                // reached as soon as ONE of its constellations was compared.
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
                            // Card CSL's own population is the BAND-1 PHASE, which is
                            // the pick the whole estimator hangs on; the wider "any
                            // (band, obs type)" count is reported beside it rather
                            // than instead of it.
                            if (key.second == 'L' && !pick.signal.empty()
                                && pick.signal.size() >= 2 && pick.signal[1] == '1')
                                rs.rule_differs_b1.insert(stations[i].name);
                        }
                    }
                }

                // The ledger: every (satellite, signal) this station reported.  The
                // arc is keyed on the SIGNAL, which is the whole point of the card.
                const auto obs_it = obs_by_cons.find(cons);
                if (obs_it != obs_by_cons.end()) {
                    for (const RinexObservation* o : obs_it->second) {
                        if (o->satellite.empty()) continue;
                        for (const auto& [key, val] : o->measurements) {
                            (void)val;
                            if (!isObsKey(key, 'L')) continue;
                            const auto cell = cells.find(key[1]);
                            const bool modelled =
                                cell != cells.end() && cell->second.modelled_phase == key;
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
                                    modelled ? "modelled" : "tracked");
                                if (ctx.arc_ledger.eventAllowed(static_cast<int>(i),
                                                                step.transition)) {
                                    ++ctr.arc_events;
                                    traces.emitEvent(
                                        se.time, "ARC", arcTransitionName(step.transition),
                                        o->satellite, stations[i].name,
                                        static_cast<double>(step.gap_len_ep),
                                        static_cast<double>(step.generation),
                                        arcStateName(step.to),
                                        fmt::format("signal={};gen={};prev_gen={};gap_ep={};role={}",
                                                    key, step.generation, step.prev_generation,
                                                    step.gap_len_ep,
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
                                     step.generation, step.gap_len_ep, reason, "");
            if (ctx.arc_ledger.eventAllowed(sta, step.transition)) {
                ++ctr.arc_events;
                traces.emitEvent(se.time, "ARC", arcTransitionName(step.transition),
                                 sat, name, static_cast<double>(step.gap_len_ep),
                                 static_cast<double>(step.generation),
                                 arcStateName(step.to),
                                 fmt::format("signal={};gen={};gap_ep={};reason={}",
                                             sig, step.generation, step.gap_len_ep,
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
                                  const SyncedEpoch& se,
                                  const std::vector<NetStation>& stations,
                                  const RawSlipEpoch* raw,
                                  const std::set<std::pair<int, std::string>>& freed,
                                  const NetworkSolutionEpoch& sol,
                                  bool emitted) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.audit_ms);
        (void)log;
        (void)se;

        // --- (a) the arc-level tracker's band signal against modelled_phase ----
        // Per ROW here; the (station, band) CELL census is the run-end mode of the
        // same rows, which is how the audit's 46 cells were defined.
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

        // --- (c) local_carry presence against the ledger ----------------------
        for (const auto& [key, lc] : ctx.carry.local_carry) {
            (void)lc;
            ++ctr.lc_entries;
            const ArcLedger::Key ak{ std::get<0>(key), std::get<1>(key), std::get<2>(key) };
            const ArcRecord* a = ctx.arc_ledger.find(ak);
            if (a == nullptr)                   ++ctr.lc_arc_unknown;
            else if (a->state == ArcState::Dead) ++ctr.lc_arc_dead;
        }
        for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
            if (a.state != ArcState::Tracking || !a.modelled) continue;
            if (!ctx.carry.local_carry.count(
                    std::make_tuple(std::get<0>(key), std::get<1>(key), std::get<2>(key))))
                ++ctr.lc_tracking_no_entry;
        }

        // --- (d) StateManager ambiguity presence against the ledger ------------
        // The prune is NOT touched: the comparison reads the surviving key set and
        // asks the ledger what it thinks of each arc, and separately asks which
        // GAPPED arcs inside the horizon have lost their state.  That second number
        // is the one the audit calls the largest silent death path in the system.
        {
            // The ledger's key orders on the STATION index first, so each station's
            // arcs are one contiguous range.  Walking it that way keeps the class at
            // O(arcs) for the whole network instead of O(stations x arcs), which is
            // what lets the observatory stay inside the cost bar at 305 stations.
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
                    if (a == nullptr)                   ++ctr.amb_arc_unknown;
                    else if (a->state == ArcState::Gap) ++ctr.amb_arc_gapped;
                }
                for (auto it = arcs.lower_bound(ArcLedger::Key{ si, "", "" });
                     it != arcs.end() && std::get<0>(it->first) == si; ++it) {
                    if (it->second.state != ArcState::Gap || !it->second.modelled) continue;
                    if (!live.count(std::get<1>(it->first) + "_" + std::get<2>(it->first)))
                        ++ctr.amb_destroyed_in_gap;
                }
            }
        }

        // --- (e) the anchor election's first sightings against the ledger ------
        // A key that has an incumbent now and had none at the previous epoch's read
        // is what the election treated as a first sighting.  The ledger is asked
        // whether its arcs had just RETURNED, which is the distinction
        // NetworkPhaseDatum.cpp deliberately does not make today.
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
        // CARRIED support: an arc counts for its family only when it is a MODELLED
        // phase, alive in the ledger AND present in local_carry.  The raw-side count
        // would be a different quantity: the roster's arrival site is upstream of the
        // elevation mask and of every admission screen, so a satellite below the mask
        // is TRACKED and not carried, and the audit's medians are carried ones.
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
            // The families a consumer may ask for are the MODELLED signals of the
            // network, so the roster itself defines the denominator.  Built once per
            // epoch, not once per satellite.
            std::set<std::string> want_phase, want_code;
            for (std::size_t i = 0; i < rs.by_station.size(); ++i) {
                for (const auto& [cons, cells] : rs.by_station[i]) {
                    (void)cons;
                    for (const auto& [band, br] : cells) {
                        (void)band;
                        if (!br.modelled_phase.empty()) want_phase.insert(br.modelled_phase);
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

        // --- the ANSWERED half, and the CARRY-SIDE gap census ------------------
        // The gap re-entry response fires in PASS A, upstream of this pass, and its
        // own latch is `ctx.carry.relevel.free_ambs` with the cause bit `kCauseGap`.
        // The driver's slipped set is a different, wider channel (RAIM, D5, the
        // deweight release all write to it), so the narrow latch is what answers
        // "was this re-entry answered by the tier that exists for it".
        (void)freed;
        const auto& rel = ctx.carry.relevel;
        const auto answered = [&](int sta, const std::string& sat,
                                  const std::string& sig) {
            const auto k = std::make_pair(sta, sat + "_" + sig);
            if (!rel.free_ambs.count(k)) return false;
            const auto cit = rel.free_cause.find(k);
            return cit == rel.free_cause.end()
                   || (cit->second & RelevelEpochState::kCauseGap) != 0;
        };
        for (const auto& [key, a] : ctx.arc_ledger.arcs()) {
            if (a.state != ArcState::Reentered) continue;
            if (a.gap_len_ep < 2 || a.gap_len_ep > 30) continue;
            if (!ctx.carry.local_carry.count(
                    std::make_tuple(std::get<0>(key), std::get<1>(key), std::get<2>(key))))
                continue;
            if (answered(std::get<0>(key), std::get<1>(key), std::get<2>(key)))
                ++ctr.gap_2_30_answered;
        }

        // The CARRY-SIDE census, driven by the carry's own RE-STASH stamp epoch to
        // epoch.  This is the audit's own population: `network_amb_state.csv` writes
        // a row when the harvest STASHES an arc, and an entry that stays in the map
        // without being re-stashed leaves no row, so map presence would report almost
        // no gaps at all (measured: 4 against the trace's 51 over the same window).
        // The stamp is @c LocalCarry::last_seen_s, set at every stash.  Reported
        // SEPARATELY from the raw-side population above, never added to it.
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
    }

    // =====================================================================
    // The run-end table
    // =====================================================================

    void networkSignalRosterRunEnd(NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log) {
        auto& rs = ctx.roster;
        if (!rs.armed) return;
        auto& c = ctx.counters.sid;

        // Class (a), the CELL census: per (station, band) the DOMINANT tracker
        // signal over the run against that cell's modelled phase.  That is the
        // audit's own definition of a mismatched station-band cell.
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
    }

} // namespace GnssProcessor
