/**
 * @file NetworkBiasAdmission.cpp
 * @brief Implementation of the W6 code-bias admission census (see the header).
 *
 * Seeded at the CB-ADM card (2026-07-21).  Pure scan over the raw synced-epoch
 * vector; owns no state and logs nothing (the caller logs the returned @c
 * census table — the sibling-seam house style keeps the seam free of spdlog).
 */

#include "filter/NetworkBiasAdmission.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::makeSatBiasKey / parseSatBiasKey
#include "common/SignalBand.hpp"        // classifySignalForSat, FrequencyBand
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch, RinexObservation

#include <algorithm>
#include <cmath>
#include <map>

namespace GnssProcessor {

NetworkBiasAdmission NetworkBiasAdmission::runCensus(
    const std::vector<SyncedEpoch>& synced, std::size_t i0,
    int census_epochs, int min_stations) {

    NetworkBiasAdmission out;
    // Empty window ⇒ active stays false ⇒ the registration site falls back to
    // the legacy per-epoch path (safe degradation, never a silent quarantine).
    if (synced.empty() || i0 >= synced.size() || census_epochs <= 0)
        return out;

    const std::size_t window_end =
        std::min(i0 + static_cast<std::size_t>(census_epochs), synced.size());
    const int k_eff = static_cast<int>(window_end - i0);
    if (k_eff <= 0) return out;

    // --- Pass 1: per (sat|sig) pair, tally the epochs each station supplies a
    // usable raw code observation; gather the distinct census roster. ---------
    // pair key -> (station -> # census epochs with a usable obs).
    std::map<std::string, std::map<std::string, int>> pair_station_epochs;
    std::set<std::string> roster;
    for (std::size_t idx = i0; idx < window_end; ++idx) {
        const SyncedEpoch& se = synced[idx];
        for (const auto& [station, obs_vec] : se.by_station) {
            roster.insert(station);
            for (const RinexObservation& obs : obs_vec) {
                // GLONASS has no OSB scope (FDMA per-(sat,signal) code OSB is
                // unphysical) — the network never allocates a sat-bias state
                // for it, so it must never enter the census either.
                if (obs.satellite.empty() || obs.satellite[0] == 'R') continue;
                for (const auto& [code, value] : obs.measurements) {
                    // Usable RAW CODE predicate: 3-char id, code obs ('C'), no
                    // composite '_' (belt-and-suspenders — raw RINEX codes never
                    // carry one), finite AND nonzero (ObsValueMap stores a real 0
                    // for a touched-but-empty slot, distinct from the NaN absent
                    // sentinel, so presence alone is not enough).
                    if (code.size() != 3 || code[0] != 'C') continue;
                    if (code.find('_') != std::string::npos) continue;
                    if (!std::isfinite(value) || value == 0.0) continue;
                    const std::string key =
                        StateKeys::makeSatBiasKey(obs.satellite, code);
                    ++pair_station_epochs[key][station];
                }
            }
        }
    }

    out.census_epochs_used = k_eff;
    out.census_stations    = static_cast<int>(roster.size());
    // effective_min = max(2, min(min_stations, n_census_stations)): 2 is the
    // hard estimability floor (a shared column needs >= 2 stations to separate
    // the satellite quantity from the receiver); the census-roster clamp keeps
    // 3-station dev rosters runnable instead of demanding an unreachable count.
    const int eff_min = std::max(2, std::min(min_stations, out.census_stations));
    out.effective_min = eff_min;
    // Sustained-support floor = ceil(K_eff / 2): a station "supports" a pair
    // only if it tracks it in at least half the census epochs — an absolute
    // floor that rejects flimsy 2-of-2 coverage from a gappy station.
    const int floor_ep = (k_eff + 1) / 2;
    out.sustained_floor = floor_ep;

    // --- Pass 2: tier-1 count gate. A pair is an admitted-candidate iff its
    // number of SUPPORTING stations (>= floor_ep census epochs each) reaches
    // effective_min.  Group the tier-1 candidates by satellite for tier 2. ----
    struct Row { std::string key, sat, sig; int support; bool tier1; };
    std::vector<Row> rows;
    rows.reserve(pair_station_epochs.size());
    std::map<std::string, std::vector<std::size_t>> sat_candidate_rows;  // sat -> row indices
    for (const auto& [key, per_station] : pair_station_epochs) {
        int support = 0;
        for (const auto& [st, cnt] : per_station)
            if (cnt >= floor_ep) ++support;
        const auto parts = StateKeys::parseSatBiasKey(key);
        if (!parts) continue;  // defensive — makeSatBiasKey output always parses.
        const bool tier1 = support >= eff_min;
        rows.push_back({key, parts->satellite, parts->signal, support, tier1});
        if (tier1)
            sat_candidate_rows[parts->satellite].push_back(rows.size() - 1);
    }

    // --- Tier 2: IF-pinnability. A satellite's tier-1 candidates are admitted
    // only if they span >= 2 DISTINCT carrier bands (the clock IF pin needs two
    // distinct frequencies).  A satellite failing this has ALL its candidates
    // demoted to QUARANTINED — killing the datum-orphan near-null class at the
    // source instead of letting the rank gate discover it and withhold the
    // whole epoch.  Band identity (classifySignalForSat) is the pin's carrier-
    // frequency distinctness with no modeling object needed. -------------------
    std::set<std::string> if_pinnable_sats;
    for (const auto& [sat, row_idxs] : sat_candidate_rows) {
        std::set<FrequencyBand> bands;
        for (const std::size_t ri : row_idxs) {
            const auto cls = classifySignalForSat(sat, rows[ri].sig);
            if (cls.band != FrequencyBand::Unknown) bands.insert(cls.band);
        }
        if (bands.size() >= 2) if_pinnable_sats.insert(sat);
    }

    // --- Finalize verdicts (deterministic key order from the std::map). -------
    for (const Row& r : rows) {
        Verdict v;
        if (!r.tier1)                          v = Verdict::LowCount;
        else if (!if_pinnable_sats.count(r.sat)) v = Verdict::NoIfPin;
        else                                   v = Verdict::Admitted;
        out.census.push_back({r.key, r.sat, r.sig, r.support, v});
        out.seen.insert(r.key);
        if (v == Verdict::Admitted) out.admitted.insert(r.key);
    }
    out.n_admitted           = static_cast<int>(out.admitted.size());
    out.n_quarantined_census = static_cast<int>(out.census.size()) - out.n_admitted;
    out.active               = true;
    return out;
}

} // namespace GnssProcessor
