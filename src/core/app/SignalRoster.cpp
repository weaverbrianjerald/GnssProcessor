/**
 * @file SignalRoster.cpp
 * @brief Card SID phase 1: the signal roster and its observatory (see SignalRoster.hpp).
 */
#include "app/SignalRoster.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"        // NetStation (roster + its PppModel / StateManager)
#include "filter/StateKeySchema.hpp"         // Card SID phase 3: parseAmbiguityKey (the prune's keys)
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
    // PHASE 2: the read side, and the IF pin as a writer
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
                    // The ROLE decides, not the fact that a row exists: an arc the
                    // roster does not call Modelled on this band has no carried
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
        // Tally the per-SATELLITE pins into (constellation, band) votes.
        //
        // ⚠ THE KEY IS THE ROSTER'S, NOT THE S-BASIS'S.  The datum seam keys a
        // constellation by the satellite id's first CHARACTER ("G"), while the roster
        // keys it by the system STRING PppUtils::getSystemString gives ("GPS"), which
        // is also the key of the signal-priority map the latch is decided from.
        // Tallying under the letter produces picks that match no cell at all and a
        // census of exactly zero, which is what the first armed run measured.
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
        // Resolve each (constellation, band) to ONE designation, and say so when the
        // satellites did not agree.  std::map iterates ascending and the replacement
        // is strictly-greater, so the tie break is the lexically lowest code, which is
        // the tie break the pin itself applies within a band.
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
                // The pin names a CODE.  A datum PHASE is named only where that code
                // IS this station's modelled code, in which case the phase it
                // accompanies is this cell's modelled phase; anywhere else the roster
                // says nothing rather than deriving a signal from the code's
                // attribute (the project's "reproduced, never re-derived" rule).
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

        // PHASE 4a (S1): arm the reason writers.  The buffer is the ONLY thing the
        // screens touch, so an unarmed run pays one bool test per writer call and
        // nothing else; the fold then receives a buffer that is not armed and
        // leaves the ledger's masks exactly as phase 3 wrote them.
        ctx.reasons.armed = config_.networkLedgerScreens();

        // PHASE 3 (R3b): arm the ambiguity prune's key recorder on every station,
        // so the audit pass can mark the arcs whose column the prune destroyed
        // inside a gap.  The prune itself is untouched and the recorder is a
        // per-epoch vector of strings; without the observatory it is never armed.
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
        // PHASE 4a (S1): open the epoch's REASON buffer.  Here and not at the fold,
        // because two writers (the readmit park and the gross-LOS screen) run
        // downstream of the audit pass and their records have to survive exactly
        // one epoch to reach the gap they can explain.
        arcReasonBeginEpoch(ctx.reasons, epoch_idx);
        // PHASE 3 (R4): the response's per-arc record is EPOCH-scoped.  Cleared at
        // the first seam of the epoch so it can never describe a previous one,
        // which is the failure mode the two earlier "answered" approximations had.
        ctx.carry.sid_reentry_outcome.clear();

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
                        ++ctr.excluded_signals;
                        const std::string r = reason;
                        if (r == "allowlist")        ++ctr.excl_allowlist;
                        else if (r == "not_latched") ++ctr.excl_not_latched;
                        else                         ++ctr.excl_no_latch;
                        // PHASE 4a (S1): the network signal ALLOW LIST writer.  The
                        // decision itself is taken once per run inside PppModel,
                        // which carries no station identity at all, but the roster
                        // has already reproduced it per (station, constellation,
                        // band, signal) with its reason, so the writer sits here
                        // rather than being plumbed into a shared model.  The allow
                        // list is a WHOLE-RUN exclusion, so it removes arcs that
                        // were never admitted and the expansion normally reaches
                        // nothing: that zero is reported, not hidden.
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
                                        // PHASE 2: BOTH gap lengths.  `gap_ep` is the raw
                                        // one; `adm_gap_ep` is the epochs the ADMITTED side
                                        // has missed as this raw edge is crossed (-1 when
                                        // the arc has never been admitted at all), which is
                                        // the number a carry reader needs and is not
                                        // derivable from the raw one.
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

        // The ANSWERED test, hoisted above the admitted pass because that pass's own
        // census uses it.  The gap re-entry response fires in PASS A, upstream of
        // this pass, and its own latch is `ctx.carry.relevel.free_ambs` with the
        // cause bit `kCauseGap`.  The driver's slipped set is a different, wider
        // channel (RAIM, D5, the deweight release all write to it), so the narrow
        // latch is what answers "was this re-entry answered by the tier that exists
        // for it".
        const auto& rel = ctx.carry.relevel;
        const auto answered = [&](int sta, const std::string& sat,
                                  const std::string& sig) {
            const auto k = std::make_pair(sta, sat + "_" + sig);
            if (!rel.free_ambs.count(k)) return false;
            const auto cit = rel.free_cause.find(k);
            return cit == rel.free_cause.end()
                   || (cit->second & RelevelEpochState::kCauseGap) != 0;
        };

        // --- PHASE 3 (R3a): the carry's gap prune as an ARC DEATH ---------------
        // The prune runs in the harvest seam, which has neither the trace bundle
        // nor the epoch time, so it buffers its keys in the context and they are
        // evented here.  Today that edge emits NOTHING at all (the audit's "31
        // lifecycle holders with both edges silent"); this is the DECOUPLING edge
        // of standing invariant 7 for the carry itself.
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

        // --- PHASE 2: the ADMITTED lifecycle, before every class that reads it ---
        // The feed is the station's MODELLED phase rows.  `modeled[i]` exists only
        // for satellites that survived the elevation mask, and `observed` holds only
        // the signals the model built (the canonical latch, its card SCC companions
        // and whatever the allow list admitted) — which is exactly the set
        // `local_carry` and the ambiguity states are keyed from.  The raw side, fed
        // at the build seam upstream of all of that, is a strict superset; the two
        // are reported side by side and never added.
        {
            // PHASE 4a (S1): the SLIP writer, on the one set that carries every
            // refloat vehicle.  `relevel.free_ambs` is written by the MW / LLI
            // drains, the IF-TD tier, the RAIM T3 and T4 responses, the gap
            // re-entry tiers and the de-weight release alike, each with the exact
            // (station, ambiguity key) identity, so recording it here is one site
            // instead of a bit added at every responder — and it is upstream of the
            // fold two lines below, which is the ordering the mask needs.  A freed
            // arc is only a gap opener where the free is followed by the row
            // disappearing; where it is not, the bit is simply not on a gap.
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
            // PHASE 4a (S1): the FOLD.  Unarmed the buffer is handed over all the
            // same and the machine sees `armed == false`, so every reason mask
            // stays exactly what phase 3 wrote.
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
                // PHASE 4a (S1): the per-reason gap census, read off the mask the
                // fold has just stamped.  A gap with two reasons increments both
                // bits, so the per-reason numbers do not sum to `gap_folded`; the
                // multi count says by how much.
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
                // The CARRY-SIDE census on the admitted lifecycle: a return at an arc
                // whose leveling carry is still live is the population the card's
                // 195 / 68 (and phase 1's 51 / 16) were measured on, now read off the
                // admitted arc's own edge instead of the carry's re-stash stamp.
                const bool carried =
                    ctx.carry.local_carry.count(std::make_tuple(sta, sat, sig)) > 0;
                if (step.transition == ArcTransition::Reentry && carried) {
                    ++ctr.carry_adm_gap_any;
                    if (step.gap_len_ep >= 2 && step.gap_len_ep <= 30) {
                        ++ctr.carry_adm_gap_2_30;
                        if (answered(sta, sat, sig)) ++ctr.carry_adm_gap_2_30_answered;
                    }
                }
                // PHASE 3 (R4): the UNIFIED answered definition.  The two
                // approximations above and in phase 1 inferred the outcome (a read
                // of `relevel.free_ambs`, a match on `reentry_relevel` events) and
                // disagreed by 5 of 51.  The response now RECORDS what it did per
                // arc, in both knob states, and the census reads that.  The split
                // is four-way and exhaustive: an arc with no record was never
                // looked at by any tier, which is the honest fourth bucket.
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
                        // PHASE 4a (S1): the NO-VERDICT population split by the
                        // reason mask of the GAP THAT PRECEDED the return.  Phase 3
                        // measured 87 of 301 returns in 6 h that no raw tier saw and
                        // named them "a screen-side decoupling"; this is the split
                        // that says which screen.  The mask is read off the arc
                        // rather than the step, because the step describes the
                        // RE-ENTRY while the mask was stamped at the gap and has
                        // survived it untouched.
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

        // --- PHASE 3 (R3b): the state-vector prune as a ledger MARK -------------
        // Drained AFTER the admitted pass, and the ordering is load-bearing: the
        // prune ran in PASS B of THIS epoch and destroys exactly the columns this
        // epoch did not model, so the arc it belongs to only enters its admitted
        // GAP when `admitEpoch` above closes this epoch.  Drained before that
        // advance the test `adm_state == Gap` is evaluated one epoch early and
        // never fires (measured: 507 pruned columns, 0 marked, on the first
        // field arm of this phase).  NO behaviour change: the prune is untouched
        // and nothing here writes to the solve.
        for (std::size_t i = 0; i < stations.size(); ++i) {
            if (!stations[i].sm) continue;
            for (const std::string& key : stations[i].sm->takePrunedAmbiguities()) {
                ++ctr.state_prune_keys;
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                const ArcLedger::Key ak{ static_cast<int>(i), parts->satellite,
                                         parts->signal };
                const ArcRecord* a = ctx.arc_ledger.find(ak);
                // Only a column destroyed while its ADMITTED arc is in a Gap
                // INSIDE the horizon is the silent death path the card names: past
                // the horizon the arc is dead anyway, and an arc the ledger never
                // admitted is a key it can say nothing about.
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

        // --- (c) local_carry presence against the ledger, BOTH SIDES ----------
        // The raw side is the phase-1 number and stays; the admitted side is the one
        // a carry reader would key on, and the difference between them is the
        // structural finding phase 2 exists to close.
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
        // PHASE 4a (S2): the same edges, read on the ADMITTED side and per ARC.
        // Class (h) above answers "does the family hold a gapped arc" on the raw
        // side; this answers "how many of its arcs are alive, gapped or dead", which
        // is what the generation test at the family's return is decided against.
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
            // The IDENTITY the audit says this class has never had: which family,
            // how long it had been without an incumbent, how many of its arcs had
            // just come back, and what the generation test made of it.  Throttled
            // on the ledger's own per (station, transition) holdoff so the stream
            // stays bounded at 305 stations; the counters carry the full volume.
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
            // B1's own two edges, emitted only where the behaviour half is armed so
            // an observatory run cannot be mistaken for one that acted.
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

        // ---------------------------------------------------------------
        // PHASE 2: the reader, the writer and the admitted lifecycle
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
        // PHASE 3: the prune and expiry edges, and the UNIFIED answered split
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
        // PHASE 4a: the REASON-MASK WRITERS (S1)
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
            // ⚠ NOT the phase-3 `ledger_reentry_no_verdict` population: that one is
            // every admitted return inside the CARRY horizon and is attributed by the
            // response's own line in NetworkTeardown.cpp.  This is the R4 split's
            // BRIDGED-WITH-NO-VERDICT bucket, whose population is narrowed to returns
            // after 2 to 30 missed epochs.  The two are stated apart on purpose; a
            // single "no-verdict by reason" number would silently be one or the other.
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
        // PHASE 4a / 4b: the ANCHOR ELECTION read against the ledger (S2 / B1 / B2)
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
