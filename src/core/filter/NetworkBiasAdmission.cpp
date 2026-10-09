/**
 * @file NetworkBiasAdmission.cpp
 * @brief Implementation of the W6 code-bias admission census (see the header).
 *
 * Pure scan over the raw synced-epoch vector (card CB-ADM).  Owns no state and
 * logs nothing: the caller logs the returned @c census table, which keeps this
 * seam free of spdlog like its sibling seams.
 */

#include "filter/NetworkBiasAdmission.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::makeSatBiasKey / parseSatBiasKey
#include "common/GnssIdentity.hpp"      // systemOfSat, obsTypeOf, SignalId, FrequencyBand
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch, RinexObservation

#include <algorithm>
#include <cmath>
#include <map>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunTypes.hpp"
#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

NetworkBiasAdmission NetworkBiasAdmission::runCensus(
    const std::vector<SyncedEpoch>& synced, std::size_t i0,
    int census_epochs, int min_stations) {

    NetworkBiasAdmission out;
    // Empty window ⇒ active stays false ⇒ the registration site falls back to
    // the legacy per-epoch path (nothing is quarantined without a census).
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
                // GLONASS has no OSB scope (a per-(sat,signal) code OSB is not
                // physical under FDMA); the network never allocates a sat-bias
                // state for it, so it is kept out of the census as well.
                if (obs.satellite.empty() || systemOfSat(obs.satellite) == System::GLONASS) continue;
                for (const auto& [code, value] : obs.measurements) {
                    // Usable raw code predicate: 3-char id, code obs ('C'), no
                    // composite '_' (a redundant check: raw RINEX codes never
                    // carry one), finite and nonzero.  ObsValueMap stores a real
                    // 0 for a touched-but-empty slot, distinct from the NaN
                    // absent sentinel, so presence alone is not enough.
                    if (code.size() != 3 || obsTypeOf(code) != ObsType::Code) continue;
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
    // estimability floor (a shared column needs >= 2 stations to separate the
    // satellite quantity from the receiver); the census-roster clamp keeps small
    // (e.g. 3-station) development rosters runnable instead of demanding an
    // unreachable count.
    const int eff_min = std::max(2, std::min(min_stations, out.census_stations));
    out.effective_min = eff_min;
    // Sustained-support floor = ceil(K_eff / 2): a station "supports" a pair
    // only if it tracks it in at least half the census epochs.  This absolute
    // floor rejects 2-of-2 coverage from a gappy station.
    const int floor_ep = (k_eff + 1) / 2;
    out.sustained_floor = floor_ep;

    // --- Pass 2: tier-1 count gate. A pair is an admission candidate iff its
    // number of supporting stations (>= floor_ep census epochs each) reaches
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
        if (!parts) continue;  // defensive: makeSatBiasKey output always parses.
        const bool tier1 = support >= eff_min;
        rows.push_back({key, parts->satellite, parts->signal, support, tier1});
        if (tier1)
            sat_candidate_rows[parts->satellite].push_back(rows.size() - 1);
    }

    // --- Tier 2: IF-pinnability. A satellite's tier-1 candidates are admitted
    // only if they span >= 2 distinct carrier bands (the clock IF pin needs two
    // distinct frequencies).  A satellite that fails this has all its candidates
    // demoted to quarantined, which removes the datum-orphan near-null
    // directions at the source instead of letting the rank gate find them and
    // withhold the whole epoch.  Band identity (SignalId::band) gives the
    // pin's carrier-frequency distinctness without a modeling object; codes of
    // unknown band are not counted. -------------------------------------------
    std::set<std::string> if_pinnable_sats;
    for (const auto& [sat, row_idxs] : sat_candidate_rows) {
        const System sys = systemOfSat(sat);
        std::set<FrequencyBand> bands;
        for (const std::size_t ri : row_idxs) {
            const auto signal = SignalId::fromObsCode(sys, rows[ri].sig);
            const FrequencyBand band = signal ? signal->band() : FrequencyBand::Unknown;
            if (band != FrequencyBand::Unknown) bands.insert(band);
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the KP prior and the CB-ADM code-pair admission.
int networkBiasAdmissionRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    if (config_.networkCbKaulaPrior())
        log->info("Network (Card KP): admitted-OSB physical-bound prior folded on {} epoch(s), "
                  "{} column-epoch(s) total at sigma {:g} m.  [Invariant #4: an ARMED run "
                  "reporting 0 epochs never reached a solve with the prior — check that "
                  "network.estimate_sat_code_bias is on and that code columns registered.]",
                  ctx.counters.n_kp_epochs, ctx.counters.n_kp_cols,
                  config_.networkCbKaulaSigmaM());
    if (config_.networkCbAdmission())
        log->info("Network (CB-ADM): {} code pairs admitted (shared columns), {} "
                  "quarantined at census, {} late-quarantined (first seen post-census) "
                  "— census {} (active={}).  [Invariant #4: a BDS-armed ON run with 0 "
                  "quarantined-at-census is a MISWIRE.]  Card H2 orphan window: {} "
                  "(key, epoch) pair(s) where an ADMITTED code key was per-epoch-withheld "
                  "(< 2 present stations) while its same-signal PHASE column stayed shared "
                  "— the deliberate cost of keeping the phase co-gate at CENSUS "
                  "granularity; expected 0 at 305-station density, and only actionable if "
                  "it is nonzero AND coincident with gate withholds.",
                  ctx.counters.n_cb_admitted, ctx.counters.n_cb_quarantined_census,
                  ctx.counters.n_cb_late_quarantined,
                  ctx.admission.active ? "ran" : "did-not-run", ctx.admission.active,
                  ctx.counters.n_cbadm_phase_orphan_key_epochs);
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
