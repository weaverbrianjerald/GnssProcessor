/**
 * @file SignalRoster.cpp
 * @brief The signal roster, its builders and its observatory (card SID, from phase 1;
 *        see SignalRoster.hpp).
 */
#include "app/SignalRoster.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"           // obsTypeOf, systemName, systemOfSat, ObservableId
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
#include <iterator>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include <memory>
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "app/NetworkSetupResult.hpp"

namespace GnssProcessor {

    namespace {

        /// @brief Is this a usable three-or-more character key of one observation type
        ///        (a band digit in the second character)?  The pure builders below get
        ///        no constellation, so the band digit is read from the key itself.
        bool isObsKey(const std::string& k, ObsType type) {
            return k.size() >= 3 && obsTypeOf(k) == type && k[1] >= '0' && k[1] <= '9';
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

    }  // namespace

    // =====================================================================
    // Census class (f): a family's carried support as a histogram
    // =====================================================================

    void RosterFamilySupport::add(int n) {
        if (n < 0) return;
        const auto idx = static_cast<std::size_t>(n);
        if (idx >= epochs_by_support.size()) epochs_by_support.resize(idx + 1, 0);
        ++epochs_by_support[idx];
        ++epochs;
    }

    long long RosterFamilySupport::below(int bar) const {
        long long k = 0;
        for (std::size_t n = 0; n < epochs_by_support.size()
                                && static_cast<long long>(n) < static_cast<long long>(bar); ++n)
            k += epochs_by_support[n];
        return k;
    }

    double RosterFamilySupport::median() const {
        if (epochs <= 0) return 0.0;
        // The sample of rank r (0-based) in sorted order: the first support value whose
        // cumulative count exceeds r.
        const auto at_rank = [this](long long r) {
            long long cum = 0;
            for (std::size_t n = 0; n < epochs_by_support.size(); ++n) {
                cum += epochs_by_support[n];
                if (cum > r) return static_cast<double>(n);
            }
            return 0.0;   // unreachable: r < epochs, the sum of the counts
        };
        const long long mid = epochs / 2;
        return (epochs % 2) ? at_rank(mid) : 0.5 * (at_rank(mid - 1) + at_rank(mid));
    }

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
            if (isObsKey(k, ObsType::Phase))     out[k[1]].tracked_phase.push_back(k);
            else if (isObsKey(k, ObsType::Code)) out[k[1]].tracked_code.push_back(k);
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
                        if (sig.size() != 3 || obsTypeOf(sig) != ObsType::Code || sig[1] != '1')
                            continue;
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
                        if (sig.size() != 3 || obsTypeOf(sig) != ObsType::Phase || sig[1] != '1')
                            continue;
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
            if (!isObsKey(k, ObsType::Phase)) continue;
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
                    if (key.size() < 3 || obsTypeOf(key) != ObsType::Phase) continue;
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
                const std::string cons = systemName(o.system);
                for (const auto& [key, val] : o.observed) {
                    (void)val;
                    if (key.size() < 3 || obsTypeOf(key) != ObsType::Phase) continue;
                    // The role decides, not the presence of a row: an arc the
                    // roster does not mark Modelled on this band has no carried
                    // level for the response to protect.  A key that does not parse
                    // (or an unknown constellation) is never a modelled phase.
                    const auto sig = SignalId::fromObsCode(o.system, key);
                    if (!sig
                        || signalRosterModelledPhase(rs, static_cast<int>(i), cons, sig->digit)
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
        // keys a constellation by its letter ("G"); the roster keys it by the system
        // name ("GPS", systemName), which is also the key of the signal-priority map
        // the latch is decided from.  Votes tallied under the letter would match no
        // roster cell, and the census would read zero.
        std::map<std::string, std::map<std::string, long long>> votes;
        for (const auto& [sat, pair] : if_pin_real) {
            if (sat.empty()) continue;
            const System sys = systemOfSat(sat);
            if (sys == System::Unknown) continue;
            const std::string cons = systemName(sys);
            for (const std::string* sig : { &pair.first, &pair.second }) {
                const auto code = ObservableId::parse(sys, *sig);
                if (!code || code->type != ObsType::Code) continue;
                ++votes[cons + "|" + std::string(1, code->signal.digit)][*sig];
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
                    if (br.modelled_code.empty()) ++out.unmodelled;
                }
            }
        }
        return out;
    }

    void networkSignalRosterDatumPin(
            NetworkRunContext& ctx,
            const std::map<std::string, std::pair<std::string, std::string>>& if_pin_real) {
        auto& rs = ctx.roster;
        if (!rs.active) return;
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
            ctr.datum_cells_unmodelled += w.unmodelled;
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
    // Card NOM step N3a: the decision sites and the "signals as planned" table
    // =====================================================================

    namespace {

        constexpr std::size_t siteIdx(RosterSite s) { return static_cast<std::size_t>(s); }

        /// The declared site texts, one row per RosterSite in enum order (held by the
        /// static_assert below).  The table prints them beside the counts, so a reader
        /// learns what each number compares without opening the code.
        constexpr std::array<RosterSiteSpec, kRosterSites> kSiteSpecs{ {
            { RosterSite::CanonicalLatch, "canonical_latch", "signal",
              "the station's canonical latch (PppModel::exportCanonicalSignals), which "
              "the roster copies",
              "roster cells checked against the latch",
              "the cell's modelled phase or code is not the latch's (bar 0: any other "
              "count is a defect of SignalRoster.cpp)" },
            { RosterSite::LatchRule, "latch_rule", "signal",
              "processing.canonical_signal_rule (PppModel::decideCanonicalPicks): the "
              "priority rule against the count rule",
              "stations whose first-epoch candidate list was compared",
              "the two rules latch a different signal on at least one (band, "
              "observation type)" },
            { RosterSite::SlipTrackerBand, "slip_tracker_band", "signal",
              "the arc-level slip tracker's band pair (networkComputeRawObservables; "
              "card NOM section 18 E9)",
              "tracker band slots on a band with a modelled phase",
              "the slot names a phase other than the modelled one" },
            { RosterSite::IfPinPair, "if_pin_pair", "signal",
              "the S-basis ionosphere-free pin pair (NetworkSbasis::selectIfPinPair; "
              "card NOM section 18 A1)",
              "roster cells with a modelled code, given the pinned code of their "
              "(constellation, band)",
              "the pinned code is not the station's modelled code" },
            { RosterSite::WirePhaseBias, "wire_phase_bias", "signal",
              "the phase-bias families the emitter writes (products.txt P records)",
              "satellite-epochs with a clock record, times the modelled phase signals",
              "no phase bias for the signal" },
            { RosterSite::WireCodeBias, "wire_code_bias", "signal",
              "the code-bias families the emitter writes (products.txt B records)",
              "satellite-epochs with a clock record, times the modelled code signals",
              "no code bias for the signal" },
            { RosterSite::LocalCarry, "local_carry", "arc",
              "the leveling carry (local_carry), keyed by (station, satellite, signal)",
              "carry entries, summed over epochs",
              "the entry's arc is dead, or was never admitted, in the ledger's "
              "admitted lifecycle" },
            { RosterSite::AmbiguityStates, "ambiguity_states", "arc",
              "the station state vectors' ambiguity columns (StateManager)",
              "ambiguity states, summed over epochs",
              "the state is alive while its admitted arc is in a gap, or it has no "
              "admitted arc" },
            { RosterSite::AnchorElection, "anchor_election", "arc",
              "the phase-datum anchor election (networkElectPhaseAnchors)",
              "first sightings (a family with an incumbent now and none at the "
              "previous epoch)",
              "the family's arcs had just returned: a return read as a first sighting" },
            { RosterSite::GapReentry, "gap_reentry", "arc",
              "the gap re-entry response (the gap re-entry tiers of NetworkSlipQc.cpp)",
              "admitted returns after 2 to 30 missed epochs",
              "no response tier examined the return (bridged with no verdict)" },
            { RosterSite::CoverageGate, "coverage_gate", "arc",
              "the phase-datum coverage gate's drop edge",
              "drop edges",
              "the dropped family still has an arc in a gap" },
        } };

        constexpr bool siteSpecsInEnumOrder() {
            for (std::size_t k = 0; k < kRosterSites; ++k)
                if (kSiteSpecs[k].site != static_cast<RosterSite>(k)) return false;
            return true;
        }
        static_assert(siteSpecsInEnumOrder(),
                      "kSiteSpecs must list every RosterSite once, in enum order");

        /// The order the excluded reasons are listed in (the roster's precedence).
        constexpr const char* kExclusionReasons[] = { "allowlist", "not_latched", "no_latch" };

        /// Signal -> count, as plan entries ordered by count (highest first), ties in
        /// signal-name order (the map's order, kept by the stable sort).
        std::vector<RosterPlanEntry> planEntries(const std::map<std::string, int>& m) {
            std::vector<RosterPlanEntry> v;
            v.reserve(m.size());
            for (const auto& [sig, n] : m) v.push_back(RosterPlanEntry{ sig, n });
            std::stable_sort(v.begin(), v.end(),
                             [](const RosterPlanEntry& a, const RosterPlanEntry& b) {
                                 return a.count > b.count;
                             });
            return v;
        }

        /// "L1C 40, L1W 18", or "-" for an empty role.
        std::string planList(const std::vector<RosterPlanEntry>& v) {
            if (v.empty()) return "-";
            std::string s;
            for (const RosterPlanEntry& e : v) {
                if (!s.empty()) s += ", ";
                s += fmt::format("{} {}", e.signal, e.count);
            }
            return s;
        }

        /// {"L1C":40,"L1W":18} (RINEX codes need no escaping).
        std::string planJsonMap(const std::vector<RosterPlanEntry>& v) {
            std::string s = "{";
            for (std::size_t k = 0; k < v.size(); ++k)
                s += fmt::format("{}\"{}\":{}", k ? "," : "", v[k].signal, v[k].count);
            return s + "}";
        }

        /// One band row of the table: left-aligned columns, trailing blanks removed.
        std::string planRow(const std::string& system, const std::string& band,
                            const std::string& role, const std::string& signals) {
            std::string s = fmt::format("{:<8} {:<4} {:<13} {}", system, band, role, signals);
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        }

        /// One decision-site row of the table: the name, four right-aligned counts,
        /// then the declared texts; trailing blanks removed.
        std::string siteRow(const std::string& name, const std::string& ce,
                            const std::string& de, const std::string& cr,
                            const std::string& dr, const std::string& tail) {
            std::string s = fmt::format("{:<18} {:>14} {:>14} {:>12} {:>12}  {}",
                                        name, ce, de, cr, dr, tail);
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        }

        /// The rule this run latched by, as the latch-rule row states it.  A station
        /// list scopes the rule and decides on its own (PppModel::canonicalRuleForStation).
        std::string latchRuleText(const RosterPlanHeader& hdr) {
            if (hdr.latch_rule_priority_stations > 0)
                return fmt::format(
                    "processing.canonical_signal_priority_stations lists {} station(s), "
                    "which latch by the priority rule; every other station latches by the "
                    "count rule (processing.canonical_signal_rule = {} does not apply)",
                    hdr.latch_rule_priority_stations, hdr.latch_rule);
            return fmt::format("processing.canonical_signal_rule = {}, so every station "
                               "latched by the {} rule and the band rows follow it",
                               hdr.latch_rule, hdr.latch_rule);
        }

        /// The header fields every table of the run shares: the span of epochs the
        /// roster was built over so far and the run's latch rule.  The caller sets the
        /// scope and the epoch the band rows describe.
        RosterPlanHeader planHeaderOf(const SignalRoster& rs, const GnssConfig& config_) {
            RosterPlanHeader h;
            h.epochs     = rs.epochs;
            h.week_first = rs.week_first;
            h.tow_first  = rs.tow_first;
            h.week_last  = rs.week_last;
            h.tow_last   = rs.tow_last;
            h.latch_rule = config_.canonicalSignalRule() == CanonicalSignalRule::Priority
                               ? "priority" : "count";
            h.latch_rule_priority_stations =
                static_cast<int>(config_.canonicalSignalPriorityStations().size());
            return h;
        }

    }  // namespace

    const std::array<RosterSiteSpec, kRosterSites>& signalRosterSiteSpecs() {
        return kSiteSpecs;
    }

    RosterSiteCounts signalRosterSiteDelta(const RosterSiteCounts& now,
                                           const RosterSiteCounts& start) {
        RosterSiteCounts d{};
        for (std::size_t k = 0; k < kRosterSites; ++k) {
            d[k].compared = now[k].compared - start[k].compared;
            d[k].disagree = now[k].disagree - start[k].disagree;
        }
        return d;
    }

    RosterSiteCounts signalRosterSiteCounts(const NetworkSignalRosterCounters& c,
                                            const SignalRoster& rs) {
        RosterSiteCounts n{};
        const auto put = [&n](RosterSite s, long long compared, long long disagree) {
            n[siteIdx(s)] = RosterSiteCount{ compared, disagree };
        };
        const auto total = [](const std::map<std::string, long long>& m) {
            long long t = 0;
            for (const auto& [sig, v] : m) { (void)sig; t += v; }
            return t;
        };
        put(RosterSite::CanonicalLatch, c.latch_cells_checked, c.roster_latch_disagree);
        put(RosterSite::LatchRule, static_cast<long long>(rs.rule_reached.size()),
            static_cast<long long>(rs.rule_differs.size()));
        put(RosterSite::SlipTrackerBand, c.trk_rows, c.trk_rows_mismatch);
        // A cell with no modelled code (the roster of a constellation's first epoch,
        // built before its latch) has nothing to compare the pin with.
        put(RosterSite::IfPinPair, c.datum_cells - c.datum_cells_unmodelled,
            c.datum_cells_off_model - c.datum_cells_unmodelled);
        put(RosterSite::WirePhaseBias, total(c.wire_pb_present) + total(c.wire_pb_missing),
            total(c.wire_pb_missing));
        put(RosterSite::WireCodeBias, total(c.wire_cb_present) + total(c.wire_cb_missing),
            total(c.wire_cb_missing));
        put(RosterSite::LocalCarry, c.lc_entries, c.lc_adm_arc_dead + c.lc_adm_arc_unknown);
        put(RosterSite::AmbiguityStates, c.amb_states,
            c.amb_adm_arc_gapped + c.amb_adm_arc_unknown);
        put(RosterSite::AnchorElection, c.anchor_first_sightings,
            c.anchor_first_sighting_reentry);
        put(RosterSite::GapReentry, c.ans_total, c.ans_bridged_nv);
        put(RosterSite::CoverageGate, c.gate_drop_edges, c.gate_drop_edges_gapped);
        return n;
    }

    bool RosterPlan::hasModelledRole() const {
        return std::any_of(bands.begin(), bands.end(),
                           [](const RosterPlanBand& b) { return b.stations_modelling > 0; });
    }

    RosterPlan signalRosterPlanOf(
            const std::vector<StationRoster>& by_station,
            const std::map<std::string, std::map<std::string, long long>>& datum_votes) {
        // Per (constellation, band): every role's signals counted over the stations.
        struct Tally {
            int reporting{ 0 };
            int modelling{ 0 };
            std::map<std::string, int> phase, code, extra, companion, datum_code, datum_phase;
            std::map<std::string, std::map<std::string, int>> excluded;  // reason -> signal -> stations
        };
        std::map<std::pair<std::string, char>, Tally> tally;
        RosterPlan plan;
        for (const StationRoster& sr : by_station) {
            if (!sr.empty()) ++plan.stations;
            for (const auto& [cons, cells] : sr) {
                for (const auto& [band, br] : cells) {
                    Tally& t = tally[{ cons, band }];
                    if (!br.tracked_phase.empty() || !br.tracked_code.empty()) ++t.reporting;
                    if (!br.modelled_phase.empty() || !br.modelled_code.empty()) ++t.modelling;
                    if (!br.modelled_phase.empty()) ++t.phase[br.modelled_phase];
                    if (!br.modelled_code.empty())  ++t.code[br.modelled_code];
                    for (const std::string& s : br.modelled_extra) ++t.extra[s];
                    for (const std::string& s : br.companion_code) ++t.companion[s];
                    if (!br.datum_code.empty())  ++t.datum_code[br.datum_code];
                    if (!br.datum_phase.empty()) ++t.datum_phase[br.datum_phase];
                    for (const auto& [sig, reason] : br.excluded) ++t.excluded[reason][sig];
                }
            }
        }
        for (auto& [key, t] : tally) {
            RosterPlanBand b;
            b.constellation      = key.first;
            b.band               = key.second;
            b.stations_reporting = t.reporting;
            b.stations_modelling = t.modelling;
            b.phase       = planEntries(t.phase);
            b.code        = planEntries(t.code);
            b.extra_phase = planEntries(t.extra);
            b.companion   = planEntries(t.companion);
            b.datum_code  = planEntries(t.datum_code);
            b.datum_phase = planEntries(t.datum_phase);
            const auto vit = datum_votes.find(key.first + "|" + std::string(1, key.second));
            if (vit != datum_votes.end()) {
                std::map<std::string, int> votes;
                for (const auto& [sig, n] : vit->second) votes[sig] = static_cast<int>(n);
                b.datum_votes = planEntries(votes);
            }
            // The known reasons in the roster's precedence order, then any other (none
            // exists today; one added later is listed rather than dropped).
            for (const char* reason : kExclusionReasons) {
                const auto eit = t.excluded.find(reason);
                if (eit != t.excluded.end())
                    b.excluded.emplace_back(reason, planEntries(eit->second));
            }
            for (const auto& [reason, sigs] : t.excluded) {
                const bool known = std::any_of(std::begin(kExclusionReasons),
                                               std::end(kExclusionReasons),
                                               [&reason](const char* r) { return reason == r; });
                if (!known) b.excluded.emplace_back(reason, planEntries(sigs));
            }
            plan.bands.push_back(std::move(b));
        }
        return plan;
    }

    std::vector<std::string> signalRosterPlanTable(const RosterPlan& plan,
                                                   const RosterSiteCounts& epoch,
                                                   const RosterSiteCounts& run,
                                                   const RosterPlanHeader& hdr) {
        std::vector<std::string> out;
        const std::string run_span =
            hdr.epochs > 0
                ? fmt::format("{} epoch(s), week {} TOW {:.1f} to week {} TOW {:.1f}",
                              hdr.epochs, hdr.week_first, hdr.tow_first, hdr.week_last,
                              hdr.tow_last)
                : std::string("no epoch built");
        if (hdr.audited)
            out.push_back(fmt::format(
                "{}: the band rows are the roster built at week {} TOW {:.1f} ({} "
                "station(s) with a cell). A number after a signal counts the stations "
                "holding that role, except after 'pinned by', where it counts the "
                "satellites whose ionosphere-free pin used that code; a datum phase is "
                "named only at a station whose modelled code is the pinned code. The "
                "decision-site rows count that epoch (epoch columns) and the run so far "
                "(run columns: {})",
                hdr.scope, hdr.week, hdr.tow, plan.stations, run_span));
        else
            out.push_back(fmt::format(
                "{}: no epoch reached the audit, so there is no epoch to describe: no band "
                "row is printed and the epoch columns read '-'. The run columns count the "
                "run so far ({})",
                hdr.scope, run_span));
        if (hdr.audited) out.push_back(planRow("system", "band", "role", "signals"));
        for (const RosterPlanBand& b : plan.bands) {
            if (!hdr.audited) break;
            const std::string band(1, b.band);
            const auto row = [&](const char* role, const std::string& text) {
                out.push_back(planRow(b.constellation, band, role, text));
            };
            row("stations", fmt::format("{} report the band, {} model it",
                                        b.stations_reporting, b.stations_modelling));
            row("phase", planList(b.phase));
            row("code", planList(b.code));
            row("second phase", planList(b.extra_phase));
            row("companion", planList(b.companion));
            if (b.datum_code.empty() && b.datum_votes.empty() && b.datum_phase.empty())
                row("datum", "-");
            else
                row("datum", fmt::format("code: {} | pinned by: {} | phase: {}",
                                         planList(b.datum_code), planList(b.datum_votes),
                                         planList(b.datum_phase)));
            std::string excl;
            for (const auto& [reason, sigs] : b.excluded) {
                if (!excl.empty()) excl += "; ";
                excl += fmt::format("{}: {}", reason, planList(sigs));
            }
            row("excluded", excl.empty() ? std::string("-") : excl);
        }
        out.push_back(siteRow("decision site", "compared_epoch", "disagree_epoch",
                              "compared_run", "disagree_run",
                              "group | where it is decided | compared | disagreement"));
        for (const RosterSiteSpec& s : kSiteSpecs) {
            const RosterSiteCount& e = epoch[siteIdx(s.site)];
            const RosterSiteCount& r = run[siteIdx(s.site)];
            std::string tail = fmt::format("{} | {} | {} | {}", s.group, s.where, s.compared,
                                           s.disagreement);
            // The latch-rule row compares two rules; which one built the band rows is
            // this run's configuration, so the row names it.
            if (s.site == RosterSite::LatchRule)
                tail += " | this run: " + latchRuleText(hdr);
            if (r.compared == 0)
                tail += " | note: no case compared in the run columns, so 0 is not an "
                        "agreement";
            const auto epochCell = [&hdr](long long v) {
                return hdr.audited ? std::to_string(v) : std::string("-");
            };
            out.push_back(siteRow(s.name, epochCell(e.compared), epochCell(e.disagree),
                                  std::to_string(r.compared), std::to_string(r.disagree),
                                  tail));
        }
        out.push_back("not in this table: the other band-role and signal-choice sites of "
                      "card NOM section 18 (groups A to G), which have no counter against "
                      "the roster yet; step N3b adds one per site");
        return out;
    }

    std::string signalRosterPlanJson(const RosterPlan& plan, const RosterSiteCounts& epoch,
                                     const RosterSiteCounts& run, const RosterPlanHeader& hdr,
                                     const std::string& scope_key) {
        std::string js = fmt::format(
            "{{\"record\":\"signals_as_planned\",\"scope\":\"{}\",\"audited\":{},"
            "\"week\":{},\"tow\":{},\"epochs\":{},\"stations\":{},"
            "\"canonical_signal_rule\":\"{}\",\"canonical_signal_priority_stations\":{},"
            "\"bands\":[",
            scope_key, hdr.audited ? "true" : "false",
            hdr.audited ? std::to_string(hdr.week) : std::string("null"),
            hdr.audited ? fmt::format("{:.1f}", hdr.tow) : std::string("null"),
            hdr.epochs, plan.stations, hdr.latch_rule, hdr.latch_rule_priority_stations);
        for (std::size_t k = 0; k < plan.bands.size(); ++k) {
            const RosterPlanBand& b = plan.bands[k];
            std::string excl = "{";
            for (std::size_t r = 0; r < b.excluded.size(); ++r)
                excl += fmt::format("{}\"{}\":{}", r ? "," : "", b.excluded[r].first,
                                    planJsonMap(b.excluded[r].second));
            excl += "}";
            js += fmt::format(
                "{}{{\"constellation\":\"{}\",\"band\":\"{}\",\"stations_reporting\":{},"
                "\"stations_modelling\":{},\"phase\":{},\"code\":{},\"second_phase\":{},"
                "\"companion\":{},\"datum_code\":{},\"datum_pinned_by\":{},"
                "\"datum_phase\":{},\"excluded\":{}}}",
                k ? "," : "", b.constellation, b.band, b.stations_reporting,
                b.stations_modelling, planJsonMap(b.phase), planJsonMap(b.code),
                planJsonMap(b.extra_phase), planJsonMap(b.companion),
                planJsonMap(b.datum_code), planJsonMap(b.datum_votes),
                planJsonMap(b.datum_phase), excl);
        }
        js += "],\"sites\":[";
        for (std::size_t k = 0; k < kRosterSites; ++k) {
            const RosterSiteSpec& s = kSiteSpecs[k];
            const std::string ep =
                hdr.audited ? fmt::format("{{\"compared\":{},\"disagree\":{}}}",
                                          epoch[k].compared, epoch[k].disagree)
                            : std::string("null");
            js += fmt::format(
                "{}{{\"site\":\"{}\",\"group\":\"{}\",\"epoch\":{},"
                "\"run\":{{\"compared\":{},\"disagree\":{}}}}}",
                k ? "," : "", s.name, s.group, ep, run[k].compared, run[k].disagree);
        }
        return js + "]}";
    }

    // =====================================================================
    // Arming
    // =====================================================================

    void networkSignalRosterArm(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::shared_ptr<spdlog::logger>& log,
                                const std::vector<NetStation>& stations) {
        auto& rs = ctx.roster;
        // Every network run builds the roster and the arc ledger (card NOM step N3a).
        // network.signal_roster, which armed them before, is the switch of the
        // ledger's trace only: what is built and counted does not depend on it.
        rs.active = true;
        rs.trace  = config_.networkSignalRoster();

        const auto& nc = config_.getNetworkConfig();
        rs.horizon_pl_s      = config_.networkPhaseLevelingMaxGapS();
        rs.horizon_pbhold_s  = nc.emit_pb_hold_max_gap_s;
        rs.horizon_readmit_s = config_.networkReadmitSeedHorizonS();
        rs.horizon_s = std::max(rs.horizon_pl_s,
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
        // of strings that the audit pass drains every epoch.
        for (const auto& st : stations)
            if (st.sm) st.sm->setRecordPrunedAmbiguities(true);

        log->info("Network (card SID / SIGNAL ROSTER + ARC LEDGER, card NOM step N3a): "
                  "built on every epoch.  The roster (per station, constellation and "
                  "band: the tracked, modelled, companion, excluded and datum signals, "
                  "copied from the canonical latch) and the arc ledger are read by the "
                  "disagreement census, the 'signals as planned' table and the SID "
                  "readers armed by their own knobs, and by nothing in the solve "
                  "otherwise.  {} producer station(s).  Ledger horizon {:.0f} s = max("
                  "phase_leveling_max_gap_s {:.0f}, "
                  "emit_pb_hold_max_gap_s {:.0f}, readmit_seed_horizon_s {:.0f}); "
                  "forget window {} processed epoch(s); ARC event holdoff {} epoch(s) "
                  "per (station, transition), births suppressed on the first epoch.  "
                  "Ledger trace (network.signal_roster): {}.",
                  n, rs.horizon_s, rs.horizon_pl_s,
                  rs.horizon_pbhold_s, rs.horizon_readmit_s,
                  kArcLedgerForgetEp, kArcLedgerEventHoldoffEp,
                  rs.trace ? "ON (logs/network_arc_ledger.csv, one row per arc "
                             "transition, and the ARC event rows in network_events.csv)"
                           : "OFF (no ledger row is written; the census does not "
                             "depend on it)");
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

    void networkBuildSignalRoster(NetworkRun& run, const NetworkEpoch& ep) {
        // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
        //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes
        //      bind through the output parameters and are marked `// written`. ----
        NetworkRunContext& ctx = run.ctx;
        const GnssConfig& config_ = run.config;
        const std::shared_ptr<spdlog::logger>& log = run.log;
        NetworkRunTraces& traces = run.traces;
        const SyncedEpoch& se = ep.se;
        const std::vector<NetStation>& stations = run.setup.stations;
        // ---- End of the bindings ----
        auto& rs = ctx.roster;
        if (!rs.active) return;                // a context no run set up (a unit test)
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.build_ms);

        // Open the epoch's decision-site census (card NOM step N3a): the audit pass
        // states the epoch's own counts as the difference from this point.
        rs.site_epoch_start = signalRosterSiteCounts(ctr, rs);
        // The datum seam writes this epoch's pin and votes.  Cleared here as well as
        // there, so an epoch that skips the datum seam pairs its cells with no votes
        // rather than with the previous epoch's.
        rs.datum_pin.clear();
        rs.datum_votes.clear();
        if (rs.epochs == 0) {
            rs.week_first = se.time.week;
            rs.tow_first  = se.time.tow;
        }
        rs.week_last = se.time.week;
        rs.tow_last  = se.time.tow;

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
                if (o.system == System::Unknown) continue;
                const std::string cons = systemName(o.system);
                if (!prio.count(cons)) continue;
                auto& tr = tracked_by_cons[cons];
                for (const auto& [key, val] : o.measurements) {
                    (void)val;
                    if (isObsKey(key, ObsType::Phase) || isObsKey(key, ObsType::Code))
                        tr.push_back(key);
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
                        ++ctr.latch_cells_checked;
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
                            const auto pick_sig =
                                SignalId::fromObsCode(systemFromName(cons), pick.signal);
                            if (key.second == 'L' && pick_sig && pick_sig->digit == '1')
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
                            if (!isObsKey(key, ObsType::Phase)) continue;
                            const auto sig = SignalId::fromObsCode(o->system, key);
                            const auto cell = sig ? cells.find(sig->digit) : cells.end();
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
                                if (rs.trace) {
                                    ++ctr.arc_trace_rows;
                                    traces.writeArcLedgerRow(
                                        se.time, stations[i].name, o->satellite, key,
                                        arcStateName(step.from), arcStateName(step.to),
                                        step.generation, step.gap_len_ep, "",
                                        modelled ? "modelled" : "tracked",
                                        "raw", arcStateName(step.adm_state),
                                        step.adm_gap_len_ep);
                                }
                                // The holdoff advances whether or not the trace is
                                // written, so the ARC rows of the SID readers, which
                                // share it, are throttled alike in both states of the
                                // trace switch.
                                const bool ev = ctx.arc_ledger.eventAllowed(
                                    static_cast<int>(i), step.transition);
                                if (ev && rs.trace) {
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
            if (rs.trace) {
                ++ctr.arc_trace_rows;
                traces.writeArcLedgerRow(se.time, name, sat, sig,
                                         arcStateName(step.from), arcStateName(step.to),
                                         step.generation, step.gap_len_ep, reason, "",
                                         "raw", arcStateName(step.adm_state),
                                         step.adm_gap_len_ep);
            }
            const bool ev = ctx.arc_ledger.eventAllowed(sta, step.transition);
            if (ev && rs.trace) {
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

    void networkSignalRosterAudit(NetworkRun& nrun, const NetworkEpoch& nep) {
        // ---- From the run and the epoch (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py,
        //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes
        //      bind through the output parameters and are marked `// written`. ----
        // The frozen body declares or uses `ep`, `run`, so this definition names the parameters
        // nep, nrun (specification 3.2).
        NetworkRunContext& ctx = nrun.ctx;
        const GnssConfig& config_ = nrun.config;
        const std::shared_ptr<spdlog::logger>& log = nrun.log;
        NetworkRunTraces& traces = nrun.traces;
        const SyncedEpoch& se = nep.se;
        const std::vector<NetStation>& stations = nrun.setup.stations;
        const std::vector<std::vector<PppObservation>>& modeled = nep.pass_a.modeled;
        const std::vector<bool>& present = nep.pass_a.present;
        const RawSlipEpoch* raw = nrun.engines.slip_qc.epoch.computed ? &nrun.engines.slip_qc.epoch : nullptr;
        const std::set<std::pair<int, std::string>>& freed = nep.funnel.carry_slipped_ambs;
        const NetworkSolutionEpoch& sol = nep.prep.sol;
        bool emitted = nep.emitted;
        // ---- End of the bindings ----
        auto& rs = ctx.roster;
        if (!rs.active) return;
        auto& ctr = ctx.counters.sid;
        Stopwatch watch(ctr.audit_ms);
        // The epoch's continuous-GPS seconds, the unit every carry horizon is in.
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);

        // The answered test, placed above the admitted pass because that pass's
        // census uses it.  The gap re-entry response fires in PASS A, upstream of
        // this pass, and its latch is `ctx.carry.relevel.free_ambs` with the cause
        // bit `kCauseGap` (an entry with no recorded cause also counts).  The
        // driver's slipped set is a wider channel (RAIM and D5 both write to it), so
        // the narrow latch is what shows whether a re-entry was
        // answered by the tier built for it.
        const auto& rel = ctx.carry.relevel;
        const auto answered = [&](int sta, const std::string& sat,
                                  const std::string& sig) {
            const auto k = std::make_pair(sta, StateKeys::makeAmbiguityKey(sat, sig));
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
            if (rs.trace) {
                ++ctr.adm_trace_rows;
                traces.writeArcLedgerRow(se.time, name, sat, sig,
                                         a ? arcStateName(a->adm_state) : "absent",
                                         "dead", a ? a->adm_generation : 0,
                                         a ? a->adm_gap_len_ep : 0, "horizon", "carry",
                                         "carry", a ? arcStateName(a->adm_state) : "absent",
                                         a ? a->admittedGapEp(ctx.arc_ledger.epochIndex())
                                           : -1);
            }
            const bool ev = ctx.arc_ledger.eventAllowed(sta, ArcTransition::Death);
            if (ev && rs.trace) {
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
                    const auto parts = StateKeys::parseAmbiguityKey(amb);
                    if (!parts) continue;
                    const ArcLedger::Key k{ sta, parts->satellite, parts->signal };
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
                if (rs.trace) {
                    ++ctr.adm_trace_rows;
                    traces.writeArcLedgerRow(se.time, name, sat, sig,
                                             arcStateName(step.from), arcStateName(step.to),
                                             step.generation, step.gap_len_ep, reason, "",
                                             "adm", arcStateName(step.adm_state),
                                             step.adm_gap_len_ep);
                }
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
        // Counted per row here; the (station, constellation, band) cell census is the
        // run-end mode of the same rows, the definition the SID audit's 46 cells use (a
        // GPS-only posture, where a cell is a (station, band) pair).
        if (raw != nullptr && raw->computed) {
            for (std::size_t i = 0; i < raw->rows.size() && i < rs.by_station.size(); ++i) {
                const StationRoster& sr = rs.by_station[i];
                for (const RawSlipObs& r : raw->rows[i]) {
                    // The cells of the row's own constellation (the roster is keyed by the
                    // constellation's name): a Galileo band-1 row is compared with the
                    // Galileo cell, not with the GPS cell of the same digit.
                    const System sys = systemOfSat(r.satellite);
                    const auto cons_it = sr.find(systemName(sys));
                    for (const std::string* s : { &r.sig1, &r.sig2 }) {
                        const auto sig = SignalId::fromObsCode(sys, *s);
                        if (!sig) continue;
                        const char band = sig->digit;
                        std::string modelled;
                        if (cons_it != sr.end()) {
                            const auto cit = cons_it->second.find(band);
                            if (cit != cons_it->second.end())
                                modelled = cit->second.modelled_phase;
                        }
                        ++rs.tracker_rows[std::make_tuple(static_cast<int>(i), sys, band, *s)];
                        if (modelled.empty()) { ++ctr.trk_rows_no_modelled; continue; }
                        rs.modelled_of_cell[std::make_tuple(static_cast<int>(i), sys, band)] = modelled;
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
                    const auto parts = StateKeys::parseAmbiguityKey(k);
                    if (!parts) continue;
                    const ArcLedger::Key ak{ si, parts->satellite, parts->signal };
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
                        live.count(StateKeys::makeAmbiguityKey(std::get<1>(it->first),
                                                               std::get<2>(it->first))) > 0;
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
                const auto parts = StateKeys::parseAmbiguityKey(amb);
                if (!parts) continue;
                const std::string& sat = parts->satellite;
                const std::string& sig = parts->signal;
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
            for (const auto& [fam, n] : support) rs.family_support[fam].add(n);
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
                for (const std::string& sig : want_code) {
                    if (s.code_bias_m.count(sig)) ++ctr.wire_cb_present[sig];
                    else                          ++ctr.wire_cb_missing[sig];
                }
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

        // --- The epoch's decision-site census and the first table (card NOM N3a) --
        // Every site counted this epoch has run by now (the IF pin writer in the solve
        // glue, the coverage gate and the anchor read after the election, every class
        // above), so the difference from the build seam's snapshot is this epoch's.
        rs.site_last_epoch =
            signalRosterSiteDelta(signalRosterSiteCounts(ctr, rs), rs.site_epoch_start);
        rs.plan_last  = signalRosterPlanOf(rs.by_station, rs.datum_votes);
        rs.audited    = true;
        rs.week_audit = se.time.week;
        rs.tow_audit  = se.time.tow;
        // The first table waits for an emitted epoch whose roster carries a modelled
        // role.  The roster is built upstream of computeModels, so a constellation's
        // first epoch has no latch yet and its cells would show every signal as
        // tracked and none as modelled; on a cold start that is the run's first epoch.
        if (!rs.plan_table_written && emitted && rs.plan_last.hasModelledRole()) {
            rs.plan_table_written = true;
            const RosterSiteCounts run = signalRosterSiteCounts(ctr, rs);
            RosterPlanHeader hdr = planHeaderOf(rs, config_);
            hdr.scope = "first emitted epoch with a modelled role";
            hdr.week  = se.time.week;
            hdr.tow   = se.time.tow;
            for (const std::string& line :
                 signalRosterPlanTable(rs.plan_last, rs.site_last_epoch, run, hdr))
                log->info("Network (signals as planned): {}", line);
            traces.appendRunManifestRecord(
                config_.getNetworkConfig(),
                signalRosterPlanJson(rs.plan_last, rs.site_last_epoch, run, hdr,
                                     "first_emitted_epoch"));
        }
    }

    // =====================================================================
    // The gate-drop edge (class h)
    // =====================================================================

    void networkSignalRosterGateDrop(NetworkRunContext& ctx, const std::string& sat,
                                     const std::string& signal) {
        if (!ctx.roster.active) return;
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
        if (!re.observe || !ctx.roster.active) return;
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

    void networkSignalRosterPlanRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                                       const std::shared_ptr<spdlog::logger>& log,
                                       NetworkRunTraces& traces) {
        const auto& rs = ctx.roster;
        if (!rs.active) return;
        // The band rows and the epoch columns are the last audited epoch, the run
        // columns every epoch the roster was built at.  The census's run-end cell
        // census of class (a) is not one of the table's sites, so the table needs
        // nothing the census computes later.
        const RosterSiteCounts run = signalRosterSiteCounts(ctx.counters.sid, rs);
        RosterPlanHeader hdr = planHeaderOf(rs, config_);
        hdr.scope   = rs.audited ? "run end (the last audited epoch)" : "run end";
        hdr.audited = rs.audited;
        hdr.week    = rs.week_audit;
        hdr.tow     = rs.tow_audit;
        for (const std::string& line :
             signalRosterPlanTable(rs.plan_last, rs.site_last_epoch, run, hdr))
            log->info("Network (signals as planned, run end): {}", line);
        traces.appendRunManifestRecord(
            config_.getNetworkConfig(),
            signalRosterPlanJson(rs.plan_last, rs.site_last_epoch, run, hdr, "run"));
    }

    void networkSignalRosterRunEnd(const NetworkRunContext& ctx,
                                   const std::shared_ptr<spdlog::logger>& log) {
        auto& rs = ctx.roster;
        if (!rs.active) return;
        auto c = ctx.counters.sid;   // a local copy: the run-end classes below are this report's own

        // Class (a), the cell census: per (station, constellation, band), the dominant
        // tracker signal over the run against that cell's modelled phase.  That is the
        // SID audit's definition of a mismatched station-band cell, per constellation.
        std::map<std::tuple<int, System, char>, std::pair<std::string, long long>> mode;
        for (const auto& [k, n] : rs.tracker_rows) {
            const auto cell = std::make_tuple(std::get<0>(k), std::get<1>(k), std::get<2>(k));
            auto& best = mode[cell];
            if (n > best.second) best = { std::get<3>(k), n };
        }
        std::set<int> mismatched_stations;
        for (const auto& [cell, best] : mode) {
            const auto mit = rs.modelled_of_cell.find(cell);
            if (mit == rs.modelled_of_cell.end() || mit->second.empty()) continue;
            ++c.trk_cells;
            if (best.first == mit->second) continue;
            ++c.trk_cells_mismatch;
            const char band = std::get<2>(cell);
            if (band == '1')      ++c.trk_cells_b1;
            else if (band == '2') ++c.trk_cells_b2;
            mismatched_stations.insert(std::get<0>(cell));
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
        for (const auto& [fam, h] : rs.family_support) {
            if (h.epochs == 0) continue;
            ++c.fam_families;
            const long long thin = h.below(3);
            if (static_cast<double>(thin) > 0.10 * static_cast<double>(h.epochs)) ++c.fam_thin;
            const auto bar = fam.find('|');
            const std::string sig = (bar == std::string::npos) ? fam : fam.substr(bar + 1);
            med_by_signal[sig].push_back(h.median());
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
                ? "  ⚠ INERT: the build seam never ran (the roster is built on every "
                  "network run), so every count in these tables is a zero with a zero "
                  "denominator; this is dead wiring, not a quiet run."
                : "");

        log->info(
            "Network (card SID phase 1 / DISAGREEMENT CLASSES): "
            "(a) TRACKER vs modelled_phase: {} of {} compared row-slot(s) name a "
            "different signal ({} slot(s) had no modelled phase to compare against); "
            "run-end CELL census {} of {} (station, constellation, band) cell(s) whose DOMINANT "
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
            "pb_hold {:.0f}, readmit {:.0f})], forget {} epoch(s).  "
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
            "retained record(s).  {}.  COST: build {:.1f} ms total "
            "({:.2f} ms/epoch), audit {:.1f} ms total ({:.2f} ms/epoch).{}",
            rs.horizon_s, rs.horizon_pl_s, rs.horizon_pbhold_s,
            rs.horizon_readmit_s, kArcLedgerForgetEp,
            c.arc_birth, c.arc_gap, c.arc_reentry, c.arc_death,
            c.gap_any, c.gap_2_30, c.gap_2_30_answered,
            c.gap_2_30 - c.gap_2_30_answered,
            c.carry_gap_any, c.carry_entries_seen, c.carry_gap_2_30,
            c.carry_gap_2_30_answered, c.carry_gap_2_30 - c.carry_gap_2_30_answered,
            c.arc_live_max, c.arc_known_max,
            rs.trace
                ? fmt::format("{} trace row(s) in network_arc_ledger.csv (one per "
                              "transition), {} ARC event row(s) in network_events.csv "
                              "(holdoff {} epoch(s) per station and transition, births "
                              "suppressed on the first epoch)",
                              c.arc_trace_rows, c.arc_events, kArcLedgerEventHoldoffEp)
                : std::string("No ledger trace written: network.signal_roster, its "
                              "switch, is off, and no count above depends on it"),
            c.build_ms, c.epochs ? c.build_ms / static_cast<double>(c.epochs) : 0.0,
            c.audit_ms, c.epochs ? c.audit_ms / static_cast<double>(c.epochs) : 0.0,
            c.arc_birth == 0
                ? "  ⚠ INERT: ZERO arcs were ever born, so the ledger saw no "
                  "observation at all.  This is dead wiring, not a quiet run."
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
            "({}).  "
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
            c.adm_birth, c.adm_gap, c.adm_reentry, c.adm_death,
            rs.trace ? fmt::format("{} trace row(s), side=adm", c.adm_trace_rows)
                     : std::string("no trace row written: network.signal_roster, the "
                                   "trace switch, is off"),
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
                ? "  ⚠ INERT: ZERO admitted rows were ever fed, so the admitted "
                  "lifecycle never advanced.  This is dead wiring, not a quiet run."
                : "");

        // ---------------------------------------------------------------
        // Phase 3: the prune and expiry edges, and the unified answered split
        // ---------------------------------------------------------------
        log->info(
            "Network (card SID phase 3 / PRUNE AND EXPIRY EDGES): "
            "(R3a) the leveling carry's gap prune retired {} entr(y|ies) at "
            "phase_leveling_max_gap_s = {:.0f} s, each an ARC arc_death with "
            "reason=horizon in the ledger's trace ({}); that edge emitted NOTHING "
            "before this phase.  "
            "(R3b) the state-vector prune destroyed {} ambiguity column(s) over the "
            "run, of which {} were MARKED state_pruned because their ADMITTED arc "
            "was in a GAP inside the carry horizon (the silent death path of the "
            "audit, now on the record) and {} belonged to an arc the ledger has "
            "never admitted; {} admitted re-entry(ies) then landed on a RE-CREATED "
            "column.  The prune itself is unchanged: no behaviour moved here.{}",
            c.carry_prune_deaths, rs.horizon_pl_s,
            rs.trace ? fmt::format("{} evented, holdoff {} epoch(s) per station and "
                                   "transition", c.carry_prune_events,
                                   kArcLedgerEventHoldoffEp)
                     : std::string("none evented: network.signal_roster, the trace "
                                   "switch, is off"),
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
                    ? "  ⚠ ARMED (network.ledger_screens) AND INERT: no admitted gap "
                      "was folded at all, so the reason masks say nothing about this run."
                    : "");
            // This is not the phase-3 `ledger_reentry_no_verdict` population: that one
            // is every admitted return inside the carry horizon and is attributed by
            // the response's own line in networkSlipGapRefGateRunEnd.  This is the
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

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    // Run-end report: signals as planned (NOM N3a).
    int networkSignalRosterPlanRunEnd(const NetworkRunEndView& v) {
        const NetworkRunContext& ctx = v.ctx;
        const GnssConfig& config_ = v.config;
        const std::shared_ptr<spdlog::logger>& log = v.log;
        NetworkRunTraces& traces = v.traces;

        // Card NOM N3a: the signal roster's "signals as planned" at run end (the last
        // audited epoch's band rows, the run's decision-site census) and its manifest
        // record, beside the two tables above and before the manifest's complete record,
        // so that record stays the manifest's last.  Written on every network run.
        networkSignalRosterPlanRunEnd(ctx, config_, log, traces);
        return 0;
    }

    // Run-end report: the signal roster and the arc ledger (SID phase 1).
    int networkSignalRosterRunEnd(const NetworkRunEndView& v) {
        const NetworkRunContext& ctx = v.ctx;
        const std::shared_ptr<spdlog::logger>& log = v.log;

        // Signal-roster run-end (SID phase 1, app/SignalRoster.cpp): the roster's build
        // census with its bar-0 self-check, every disagreement class with its denominator,
        // and the arc ledger's transitions, horizon and cost.  Written on every network run
        // (card NOM step N3a; its "signals as planned" table is written above, beside the
        // parameter and contribution tables).  Read here, before the zero-emit guard, for the
        // reason the two clauses above give: the observatory's table matters most on a run
        // that produced nothing.
        networkSignalRosterRunEnd(ctx, log);
        return 0;
    }

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
