/**
 * @file NetworkChannelAll.cpp
 * @brief Implementation of the card SID phase 6b census (see the header for the
 *        contract and for which seam consumes each function).
 */
#include "filter/NetworkChannelAll.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "model/PppModel.hpp"

#include <algorithm>
#include <set>
#include <spdlog/spdlog.h>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunGroups.hpp"
#include "slip/NetworkSlipQc.hpp"
#include <memory>

namespace GnssProcessor {

bool channelAllIsExtra(const std::string& signal, const std::string& canonical,
                       bool canonical_present) {
    if (signal.size() < 2) return false;
    if (canonical.empty() || !canonical_present) return false;   // undecided latch
    if (canonical.size() < 2 || canonical[1] != signal[1]) return false;
    return canonical != signal;
}

std::vector<std::string> channelAllExtraChannels(
    const std::vector<std::string>& keys,
    const std::vector<char>& bands,
    const std::map<char, std::string>& canon_code,
    const std::map<char, std::string>& canon_phase,
    const std::vector<std::string>* allowed) {

    const std::set<std::string> key_set(keys.begin(), keys.end());
    std::vector<std::string> out;
    for (const std::string& k : key_set) {
        if (k.size() < 3) continue;
        const ObsType type = obsTypeOf(k);
        if (type != ObsType::Code && type != ObsType::Phase) continue;
        if (std::find(bands.begin(), bands.end(), k[1]) == bands.end()) continue;
        if (allowed != nullptr
            && std::find(allowed->begin(), allowed->end(), k) == allowed->end()) continue;
        const auto& canon_map = (type == ObsType::Code) ? canon_code : canon_phase;
        const auto cit = canon_map.find(k[1]);
        const std::string canon = (cit == canon_map.end()) ? std::string{} : cit->second;
        if (!channelAllIsExtra(k, canon, key_set.count(canon) != 0)) continue;
        out.push_back(k);
    }
    return out;   // std::set iterates ascending, so `out` is already sorted
}

void networkChannelAllEpoch(NetworkRunContext& ctx, const GnssConfig& config_,
                            const std::vector<NetStation>& stations,
                            const std::vector<std::vector<PppObservation>>& modeled,
                            const std::vector<bool>& present) {
    // The census also runs on a single-channel posture when a second constellation
    // is active, so its families can be read per constellation.  The run-end line is
    // then card GAL's; the three phase-6b lines are written only under `all`
    // (card GAL).
    if (!config_.networkChannelModeAll() && !networkMultiGnssActive(config_)) return;
    auto& C = ctx.counters.channel_all;
    ++C.epochs;

    // Per-(constellation, signal) tallies of this epoch only, so the census stores peaks.
    std::map<std::string, std::set<int>>         sta_tracking, sta_modelling;
    std::map<std::string, std::set<std::string>> cols;
    std::set<char> extra_code_bands;
    long long isc_stations = 0;
    std::set<std::string> isc_signals;

    for (std::size_t i = 0; i < modeled.size() && i < stations.size(); ++i) {
        if (i < present.size() && !present[i]) continue;
        // (station, satellite, band) -> how many modelled phases / codes it carries.
        std::map<std::pair<std::string, char>, std::pair<int, int>> cell;
        for (const PppObservation& ob : modeled[i]) {
            if (ob.satellite.empty() || ob.system == System::Unknown) continue;
            const std::string cons = systemName(ob.system);
            for (const auto& [sig, val] : ob.raw_observed) {
                (void)val;
                const ObsType type = obsTypeOf(sig);
                if (sig.size() < 3 || (type != ObsType::Code && type != ObsType::Phase)) continue;
                sta_tracking[cons + "|" + sig].insert(static_cast<int>(i));
            }
            for (const auto& [sig, val] : ob.observed) {
                (void)val;
                const ObsType type = obsTypeOf(sig);
                if (sig.size() < 3 || (type != ObsType::Code && type != ObsType::Phase)) continue;
                const bool phase = (type == ObsType::Phase);
                const std::string key = cons + "|" + sig;
                sta_modelling[key].insert(static_cast<int>(i));
                cols[key].insert(ob.satellite);
                // The band digit is the key's second character (the identity parser
                // would refuse an ionosphere-free key such as "C1C_C2W", which counts
                // on its first band here).
                auto& cc = cell[{ ob.satellite, sig[1] }];
                if (phase) ++cc.first; else ++cc.second;
                const bool extra = phase ? ob.second_phases.count(sig) != 0
                                         : ob.companion_codes.count(sig) != 0;
                auto& row = C.family[key];
                if (extra) {
                    ++row.rows_extra;
                    if (!phase) extra_code_bands.insert(sig[1]);
                } else {
                    ++row.rows_canonical;
                }
            }
        }
        // Cross-channel exposure (R5): a cell carrying more than one modelled attribute
        // of a band is one that every screen keyed on "SAT|<band>" or on (station,
        // satellite) acts on as a whole.
        for (const auto& [k, n] : cell) {
            (void)k;
            ++C.xchan_cells_total;
            if (n.first  > 1) ++C.xchan_phase_cells;
            if (n.second > 1) ++C.xchan_code_cells;
        }
        // The receiver inter-signal code states this station holds.
        if (stations[i].sm) {
            bool any = false;
            for (const auto& [key, idx] : stations[i].sm->getCodeBiasStates()) {
                (void)idx;
                if (const auto p = StateKeys::parseReceiverIscKey(key)) {
                    isc_signals.insert(p->signal);
                    any = true;
                }
            }
            if (any) ++isc_stations;
        }
    }

    for (const auto& [key, s] : sta_tracking)
        C.family[key].sta_tracking = std::max(C.family[key].sta_tracking,
                                              static_cast<long long>(s.size()));
    for (const auto& [key, s] : sta_modelling)
        C.family[key].sta_modelling = std::max(C.family[key].sta_modelling,
                                               static_cast<long long>(s.size()));
    for (const auto& [key, s] : cols)
        C.family[key].columns = std::max(C.family[key].columns,
                                         static_cast<long long>(s.size()));
    C.isc_station_epochs += isc_stations;
    C.isc_signals_max = std::max(C.isc_signals_max,
                                 static_cast<long long>(isc_signals.size()));
    C.extra_code_bands_max = std::max(C.extra_code_bands_max,
                                      static_cast<long long>(extra_code_bands.size()));
    long long ge2 = 0;
    for (const char b : extra_code_bands) if (b >= '2') ++ge2;
    C.extra_code_bands_ge2_max = std::max(C.extra_code_bands_ge2_max, ge2);
}

void networkChannelAllRunEnd(const NetworkRunContext& ctx, const GnssConfig& config_,
                             const std::shared_ptr<spdlog::logger>& log,
                             long long gap_extra_channel) {
    if (!config_.networkChannelModeAll()) return;
    const auto& C = ctx.counters.channel_all;
    if (C.epochs == 0) {
        log->warn("Network (card SID phase 6b, ALL-CHANNEL): armed and the census never "
                  "ran over any epoch — dead wiring, not a clean run (standing "
                  "invariant #4).");
        return;
    }
    std::string table;
    long long extra_families = 0, extra_rows = 0;
    for (const auto& [key, row] : C.family) {
        if (row.rows_extra > 0) { ++extra_families; extra_rows += row.rows_extra; }
        table += fmt::format("{}{} trk {} mdl {} col {} canon {} extra {}",
                             table.empty() ? "" : " | ", key, row.sta_tracking,
                             row.sta_modelling, row.columns, row.rows_canonical,
                             row.rows_extra);
    }
    log->info("Network (card SID phase 6b, ALL-CHANNEL FAMILY CENSUS over {} epoch(s), "
              "coverage gate network.signal_min_station_fraction = {:.3f}): {} "
              "(constellation, signal) famil(y|ies), {} of them carrying EXTRA-channel "
              "rows, {} extra row-epoch(s) in all.  Per family, peak in one epoch: "
              "stations TRACKING it, stations MODELLING it, satellite COLUMNS it "
              "reached; then row-epochs as the station's CANONICAL signal and as an "
              "EXTRA channel.  [{}]",
              C.epochs, config_.networkSignalMinStationFraction(),
              static_cast<long long>(C.family.size()), extra_families, extra_rows,
              table);
    if (extra_families == 0)
        log->warn("Network (card SID phase 6b, ALL-CHANNEL): the widening is active and "
                  "NO family ever carried an extra-channel row — every station reports "
                  "one attribute per selected band, so this run is byte-equivalent to "
                  "the single-channel posture (standing invariant #4).  Every simulated "
                  "bed as delivered reads this way by construction.");
    log->info("Network (card SID phase 6b, CROSS-CHANNEL EXPOSURE, R5): {} of {} "
              "(station, satellite, band) cell-epoch(s) carry more than one modelled "
              "PHASE and {} more than one modelled CODE.  Those are the cells a screen "
              "keyed on the BAND (NetworkMeasurementAdmission, 'SAT|<band>') or on the "
              "LINE OF SIGHT (gl_state, adm_arc, si_arc, arc_spike_streak) acts on "
              "WHOLESALE: one channel's verdict reaches every channel of the cell.  The "
              "eight pair sites the audit named select BANDS, not channels, and are "
              "unchanged.  ACTIONS taken on an extra channel are counted separately: "
              "the row-level gap re-entry tier reached {} of them (card SID phase 6b "
              "R3), and the per-tier slip census of the second-phase line above covers "
              "the rest.",
              C.xchan_phase_cells, C.xchan_cells_total, C.xchan_code_cells,
              gap_extra_channel);
    log->info("Network (card SID phase 6b, INTER-SIGNAL STATES): {} station-epoch(s) "
              "held at least one receiver inter-signal code state, naming at most {} "
              "distinct SIGNAL(s) in one epoch; extra CODE channels appeared on at most "
              "{} band digit(s) in one epoch, {} of them on band 2 or above.  A band-2 "
              "or higher extra code is the population network.sat_isc_state ({}) "
              "decides: with it OFF the RF-0d geometry-free pin asserts that column's "
              "inter-signal correction to ZERO (card B5D's class); with it ON the "
              "column is freed wherever a station bridges both signals of the "
              "satellite, which under this widening is every widened station.",
              C.isc_station_epochs, C.isc_signals_max, C.extra_code_bands_max,
              C.extra_code_bands_ge2_max,
              config_.getNetworkConfig().sat_isc_state ? "armed" : "OFF");
    if (C.extra_code_bands_ge2_max > 0 && !config_.getNetworkConfig().sat_isc_state)
        log->warn("Network (card SID phase 6b): extra CODE channels were modelled on "
                  "band 2 or above with network.sat_isc_state OFF, so each of those "
                  "satellite columns carries an ASSERTED zero inter-signal correction "
                  "(standing invariant #7's decoupling edge with nothing bounding what "
                  "the assertion absorbs).  Arm network.sat_isc_state.");
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the all-channel census (SID phase 6b).
int networkChannelAllRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const SlipQcChannel& slip_qc = v.run.engines.slip_qc;

    // All-channel family census and cross-channel exposure (SID phase 6b), beside the
    // roster's table because the two answer the same question at different
    // granularities.  Returns on its first line when the widening is not active.
    networkChannelAllRunEnd(ctx, config_, log,
                            slip_qc.counters.gap_reentry_extra_channel);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
