/**
 * @file SignalRosterSites.cpp
 * @brief The site census of card NOM step N3b, and the roster reads it measures.
 *
 * Card NOM section 18 lists the sites that decide a band role or a signal choice
 * (groups A to G).  Step N3a gave eleven of them a counter (@ref RosterSite); step N3b
 * measures more of them against the roster and moves a reader onto the roster where
 * its counter is 0 on every census arm.  The census here is the measurement: the rule
 * sites are evaluated on each audited epoch's roster, and the read sites count at the
 * site.  The sites the census cannot measure (the roster holds no role for their
 * question) are listed with their reasons in scripts/nom3_designs/N3b_decisions.md.
 *
 * Nothing in the solve reads the census; it prints at run end only.
 */
#include "network/assembly/SignalRoster.hpp"
#include "network/state/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"           // SignalId, systemFromName, systemOfSat, traitsOf
#include "network/solve/NetworkDatumGraph.hpp"      // netDatumGraphFamily (F2)
#include "network/assembly/NetworkExtraBandScope.hpp"  // networkPinBandsFor (A5 and its readers)
#include "network/runend/NetworkRunEnd.hpp"
#include "network/solve/NetworkSbasis.hpp"          // broadcastGroupDelayDescribesPair (A2), channelFamilyOf (F1)
#include "products/ProductCache.hpp"         // the BIA clock-reference signals
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <set>
#include <vector>

namespace GnssProcessor {

    const std::array<RosterN3bSpec, kRosterN3bSites>& signalRosterN3bSpecs() {
        static const std::array<RosterN3bSpec, kRosterN3bSites> specs{ {
            { RosterN3bSite::BiaPinBands, "bia_pin_bands",
              "A5-A9, A12, B3, B5, B7",
              "constellation-epochs with a datum",
              "the BIA clock-reference pin bands are not the bands of the roster's datum codes" },
            { RosterN3bSite::BiaPinSignals, "bia_pin_signals",
              "A1 against A5 (the pin pair decision)",
              "(constellation, BIA band)-epochs",
              "the BIA reference code is not the roster's datum code of that band" },
            { RosterN3bSite::GdPairBands, "gd_pair_bands",
              "A2",
              "pinned satellite-epochs",
              "the pinned pair is not the pair the broadcast group delay describes" },
            { RosterN3bSite::CanonicalPair, "canonical_pair",
              "A10",
              "(station, constellation)-epochs with modelled phases on two bands or more",
              "the two lowest modelled band digits are not 1 and 2" },
            { RosterN3bSite::TraitsPairBands, "traits_pair_bands",
              "A13",
              "constellation-epochs with a datum",
              "the first two traits bands are not the bands of the roster's datum codes" },
            { RosterN3bSite::ChannelFamily, "channel_family",
              "F1 against F2",
              "pairs of modelled signals of one observation type, per constellation-epoch",
              "\"X against the rest\" and the attribute letter group the pair differently" },
            { RosterN3bSite::LatchReadTracker, "latch_read_tracker",
              "E10 (and E9's latched pick)",
              "tracker band reads both the roster and the latch answer",
              "the roster's modelled phase is not the latched phase" },
            { RosterN3bSite::LatchReadSecondPhase, "latch_read_second_phase",
              "C3, E7 (the second-phase census's latched phase)",
              "band reads both the roster and the latch answer",
              "the roster's modelled phase is not the latched phase" },
            { RosterN3bSite::WindupRestartArcs, "windup_restart_arcs",
              "arc identity: the wind-up restart's carry targets",
              "restarts answered from the leveling carry",
              "the ledger's admitted, not-dead arcs with a carry entry are not the carry walk's" },
        } };
        static_assert(kRosterN3bSites == 9, "one spec per RosterN3bSite");
        return specs;
    }

    void signalRosterN3bCount(RosterN3bCensus& c, RosterN3bSite site, bool agree,
                              const std::string& what) {
        const std::size_t k = static_cast<std::size_t>(site);
        ++c.count[k].compared;
        if (agree) return;
        ++c.count[k].disagree;
        ++c.detail[k][what];
    }

    namespace {

        /// "1,2" for the digits of a band set, ascending.
        std::string digitsText(const std::set<char>& bands) {
            std::string out;
            for (const char b : bands) {
                if (!out.empty()) out += ',';
                out += b;
            }
            return out;
        }

        /// One constellation's pair-wise family census over a set of signals of one type.
        void familyPairs(RosterN3bCensus& c, const std::string& cons,
                         const std::set<std::string>& sigs) {
            const std::vector<std::string> v(sigs.begin(), sigs.end());
            for (std::size_t a = 0; a < v.size(); ++a)
                for (std::size_t b = a + 1; b < v.size(); ++b) {
                    const bool f1 = NetworkSbasis::channelFamilyOf(cons, v[a])
                                    == NetworkSbasis::channelFamilyOf(cons, v[b]);
                    const bool f2 = netDatumGraphFamily(v[a]) == netDatumGraphFamily(v[b]);
                    signalRosterN3bCount(c, RosterN3bSite::ChannelFamily, f1 == f2,
                                         fmt::format("{} {}~{} ({})", cons, v[a], v[b],
                                                     f1 ? "one family by X-rule"
                                                        : "one family by letter"));
                }
        }

    }  // namespace

    void signalRosterSiteCensus(const SignalRoster& rs, RosterN3bCensus& c) {
        // The roster's datum: constellation -> band digit -> the code the epoch's IF pin
        // realized there (the mode over the pinned satellites).
        std::map<std::string, std::map<char, std::string>> datum;
        for (const auto& [cb, code] : signalRosterDatumPicks(rs.datum_pin, nullptr, nullptr)) {
            const std::size_t bar = cb.find('|');
            if (bar == std::string::npos || bar + 1 >= cb.size()) continue;
            datum[cb.substr(0, bar)][cb[bar + 1]] = code;
        }
        const auto& refs = ProductCache::getInstance().getClockRefSignals();
        for (const auto& [cons, by_band] : datum) {
            const System sys = systemFromName(cons);
            if (sys == System::Unknown) continue;
            std::set<char> bands;
            for (const auto& [b, code] : by_band) {
                (void)code;
                bands.insert(b);
            }
            const ConstellationTraits& t = traitsOf(sys);
            // A5 and its readers: the BIA clock-reference pin bands.
            const NetworkPinBands pin = networkPinBandsFor(t.letter);
            if (pin.valid()) {
                const std::set<char> pb{ pin.band1, pin.band2 };
                signalRosterN3bCount(c, RosterN3bSite::BiaPinBands, pb == bands,
                                     fmt::format("{} BIA {} roster {}", cons,
                                                 digitsText(pb), digitsText(bands)));
            }
            // The pin-pair decision at the signal level: the BIA reference code per band.
            const auto rit = refs.find(t.letter);
            if (rit != refs.end()) {
                for (const std::string* sig : { &rit->second.sig1, &rit->second.sig2 }) {
                    const auto id = SignalId::fromObsCode(sys, *sig);
                    if (!id) continue;
                    const auto dit = by_band.find(id->digit);
                    if (dit == by_band.end()) continue;   // no datum on that band: not comparable
                    signalRosterN3bCount(c, RosterN3bSite::BiaPinSignals, dit->second == *sig,
                                         fmt::format("{} band {} BIA {} roster {}", cons,
                                                     id->digit, *sig, dit->second));
                }
            }
            // A13: the first two traits bands.
            if (t.n_bands >= 2) {
                const std::set<char> tb{ t.bands[0].digit, t.bands[1].digit };
                signalRosterN3bCount(c, RosterN3bSite::TraitsPairBands, tb == bands,
                                     fmt::format("{} traits {} roster {}", cons,
                                                 digitsText(tb), digitsText(bands)));
            }
        }
        // A2: every pinned satellite's pair against the group-delay pair.
        for (const auto& [sat, pair] : rs.datum_pin) {
            if (sat.empty() || pair.first.empty() || pair.second.empty()) continue;
            const bool gd =
                NetworkSbasis::broadcastGroupDelayDescribesPair(sat, pair.first, pair.second);
            signalRosterN3bCount(c, RosterN3bSite::GdPairBands, gd,
                                 fmt::format("{} {}/{}", systemName(systemOfSat(sat)),
                                             pair.first, pair.second));
        }
        // A10 per (station, constellation); F1 against F2 per constellation.
        std::map<std::string, std::set<std::string>> phases, codes;
        for (const StationRoster& sr : rs.by_station) {
            for (const auto& [cons, cells] : sr) {
                std::set<char> modelled_bands;
                for (const auto& [band, cell] : cells) {
                    if (!cell.modelled_phase.empty()) {
                        modelled_bands.insert(band);
                        phases[cons].insert(cell.modelled_phase);
                    }
                    for (const std::string& s : cell.modelled_extra) phases[cons].insert(s);
                    if (!cell.modelled_code.empty()) codes[cons].insert(cell.modelled_code);
                    for (const std::string& s : cell.companion_code) codes[cons].insert(s);
                }
                if (modelled_bands.size() < 2) continue;
                auto it = modelled_bands.begin();
                const char d0 = *it++;
                const char d1 = *it;
                signalRosterN3bCount(c, RosterN3bSite::CanonicalPair, d0 == '1' && d1 == '2',
                                     fmt::format("{} {},{}", cons, d0, d1));
            }
        }
        for (const auto& [cons, sigs] : phases) familyPairs(c, cons, sigs);
        for (const auto& [cons, sigs] : codes) familyPairs(c, cons, sigs);
    }

    std::string signalRosterModelledOr(const SignalRoster& rs, const std::string& latch,
                                       int station, const std::string& cons, char band,
                                       char obs_type, RosterN3bCensus* c,
                                       RosterN3bSite site) {
        std::string roster;
        if (station >= 0 && station < static_cast<int>(rs.by_station.size())) {
            const StationRoster& sr = rs.by_station[static_cast<std::size_t>(station)];
            const auto cit = sr.find(cons);
            if (cit != sr.end()) {
                const auto bit = cit->second.find(band);
                if (bit != cit->second.end())
                    roster = obs_type == 'C' ? bit->second.modelled_code
                                             : bit->second.modelled_phase;
            }
        }
        if (roster.empty()) {
            if (c != nullptr && !latch.empty()) ++c->fallback[static_cast<std::size_t>(site)];
            return latch;
        }
        if (c != nullptr && !latch.empty())
            signalRosterN3bCount(*c, site, roster == latch,
                                 fmt::format("{} band {} roster {} latch {}", cons, band,
                                             roster, latch));
        return roster;
    }

    // ---- Run-end reports (card NOM step TD1b): one row of kNetworkRunEndReports ----

    int networkSignalRosterSitesRunEnd(const NetworkRunEndView& v) {
        const NetworkRunContext& ctx = v.ctx;
        const std::shared_ptr<spdlog::logger>& log = v.log;
        if (!ctx.roster.active) return 0;
        const RosterN3bCensus& c = ctx.counters.sid.n3b;
        log->info("Network (roster site census, N3b): {} site(s) of card NOM section 18 "
                  "measured against the roster or the arc ledger over the run; a read site "
                  "at 0 answers as its old rule did at every read",
                  kRosterN3bSites);
        for (const RosterN3bSpec& s : signalRosterN3bSpecs()) {
            const std::size_t k = static_cast<std::size_t>(s.site);
            std::vector<std::pair<long long, std::string>> top;
            for (const auto& [what, n] : c.detail[k]) top.emplace_back(n, what);
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            std::string most;
            for (std::size_t j = 0; j < top.size() && j < 4; ++j)
                most += fmt::format("{}{} x{}", j ? "; " : "", top[j].second, top[j].first);
            log->info("Network (roster site census, N3b): {} [{}]: compared {}, disagree {}, "
                      "fallback {} (unit: {}; disagreement: {}){}{}",
                      s.name, s.sites, c.count[k].compared, c.count[k].disagree,
                      c.fallback[k], s.compared, s.disagreement,
                      most.empty() ? "" : "; most: ", most);
        }
        return 0;
    }

}  // namespace GnssProcessor
