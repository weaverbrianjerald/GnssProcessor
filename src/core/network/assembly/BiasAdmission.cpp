/**
 * @file BiasAdmission.cpp
 * @brief Implementation of the W6 code-bias admission census (see the header).
 *
 * Pure scan over the raw synced-epoch vector.  Owns no state and
 * logs nothing: the caller logs the returned @c census table, which keeps this
 * seam free of spdlog like its sibling seams.
 */

#include "network/assembly/BiasAdmission.hpp"

#include "filter/StateKeySchema.hpp"    // StateKeys::makeSatBiasKey / parseSatBiasKey
#include "common/GnssIdentity.hpp"      // systemOfSat, obsTypeOf, SignalId, FrequencyBand
#include "rinex/MultiRinexSync.hpp"     // SyncedEpoch, RinexObservation

#include <algorithm>
#include <cmath>
#include <map>

// The run-end reports at the end of this file:
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunTypes.hpp"
#include <memory>
#include <spdlog/spdlog.h>

#include "network/driver/Epoch.hpp"        // NetworkEpoch (the solve stages)
#include "network/driver/Run.hpp"          // NetworkRun
#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/SolveGlue.hpp"   // SolveDatum, NetworkPostFitHandoff
#include "network/diag/RunTraces.hpp"
#include "network/driver/Run.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "math/TimeUtils.hpp"
#include "common/GnssIdentity.hpp"

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

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

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

// ---- Solve stages (generated by scripts/nom_sg_cut.py) ----
/// The physical-bound prior on the admitted OSB columns, folded and captured (Doxygen at the declaration).
void networkFoldCodeBiasKaulaPrior(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    NetworkEstimator& est = run.engines.est;
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys =
        ep.columns.registered_code_keys;
    const bool   within_epoch_armed = ctx.knobs.within_epoch_armed;   // network.raim_within_epoch
    auto& cap_priors = ws.cap_priors;
    // ---- End of the bindings ----
    // --- Loose physical-bound prior on the admitted OSB columns ---
    // Folded after the tight gauge rows and before the rank gate, through the
    // diagonal prior channel rather than the gauge stack, so
    // plan.declared_dimension is unchanged by construction (K3).  It targets the
    // code-dominated residual direction that the phase drop-and-heal cannot
    // reach: it anchors PB columns only, so a near-null direction with zero phase
    // weight is outside its reach.  With the knob off the list is empty and
    // addSharedPrior returns immediately (byte-identical).
    if (config_.networkCbKaulaPrior()) {
        const auto kp = NetworkSbasis::buildCodeBiasKaulaPriors(
            registered_code_keys, config_.networkCbKaulaSigmaM());
        if (!kp.empty()) {
            est.contributionKind(ContributionKind::CbKaula);
            est.addSharedPrior(kp);
            // The RAIM within-epoch replay re-solves from a fresh beginEpoch();
            // a prior missing there would give the replayed solve a different
            // datum from the first one, with nothing in the log.
            if (within_epoch_armed)
                networkCapturePriors(cap_priors, ws.cap_prior_kinds, kp,
                                     ContributionKind::CbKaula);
            ++ctx.counters.n_kp_epochs;
            ctx.counters.n_kp_cols += kp.size();
            // The K2 anti-fragility margin is measured after the gate (in the
            // one-shot block there), not here: it needs lambda_max of the same
            // reduced system the gate judges, and calling reducedSharedNormals()
            // here would force the Schur reduction before the clock datum rows
            // are folded.  addSharedInfo would carry the shared-border delta
            // through the cached reduction in place, but the clock rows' w·HᵀH
            // increment is not bit-symmetric, so reducing early would make the
            // first epoch's last bits depend on whether KP is armed.
        }
    }
}

/// The one-shot K2 margin of the KP prior against the rank-gate cutoff (Doxygen at the declaration).
void networkKaulaMargin(NetworkRun& run) {
    // ---- From the run, the epoch and the solve's values (generated by
    //      scripts/nom_sg_cut.py, each line checked by scripts/nom_sg_verify.py) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkEstimator& est = run.engines.est;
    // ---- End of the bindings ----
    // The K2 anti-fragility margin of the KP prior is logged once, on the reduced system
    // after the KP prior and the clock datum rows and before the SBC rows.  lambda_max
    // here equals the gate's only on an epoch where SBC folds nothing and no clock-clone
    // view is formed (the gate then judges the clone-marginalized view).
    // The CB-ADM study rejected a loose prior
    // whose information sat at the rank cutoff, so this one must sit at least 2
    // decades above it; a margin under 2 decades logs a warning.
    if (config_.networkCbKaulaPrior() && !ctx.counters.kp_margin_logged
        && ctx.counters.n_kp_cols > 0) {
        ctx.counters.kp_margin_logged = true;
        const Eigen::MatrixXd& Nss_kp = est.reducedSharedNormals();
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es_kp(Nss_kp, Eigen::EigenvaluesOnly);
        if (es_kp.info() == Eigen::Success && es_kp.eigenvalues().size() > 0) {
            const double lmax = es_kp.eigenvalues()(es_kp.eigenvalues().size() - 1);
            const double cutoff = est.rankGateTol() * std::max(lmax, 1.0);
            const double info = 1.0 / (config_.networkCbKaulaSigmaM()
                                       * config_.networkCbKaulaSigmaM());
            const double decades = (cutoff > 0.0 && info > 0.0)
                ? std::log10(info / cutoff) : 0.0;
            if (decades >= 2.0)
                log->info("Network (Card KP): prior info {:.3e} vs rank-gate cutoff {:.3e} "
                          "— margin {:.1f} decades ABOVE the cutoff (K2 requires >= 2.0).  "
                          "sigma {:g} m on the admitted OSB columns.",
                          info, cutoff, decades, config_.networkCbKaulaSigmaM());
            else
                log->warn("Network (Card KP): prior info {:.3e} sits only {:.1f} decades "
                          "above the rank-gate cutoff {:.3e} — BELOW the K2 minimum of 2.0.  "
                          "This is the exact configuration the CB-ADM card rejected (prior "
                          "information AT the cutoff either trips the gate or survives by "
                          "coincidence).  TIGHTEN network.cb_kaula_sigma_m or re-place "
                          "network.rank_gate_tol from this config's estimability dump before "
                          "trusting the result.", info, decades, cutoff);
        }
    }
}
// ---- End of the solve stages ----

// Setup builder; documented at its declaration in network/assembly/BiasAdmission.hpp.
void networkCbAdmissionCensus(NetworkRun& run) {
    // ---- From the run (generated by scripts/nom_ep4_cut.py, each line
    //      checked by scripts/nom_ep4_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    MultiRinexSync& sync = *run.setup.sync;
    const auto& synced = sync.getSyncedEpochs();
    const double start_offset_s = ctx.knobs.start_offset_s;  // testing fast-forward (0 = off)
    // ---- End of the bindings ----
    // The code-bias admission census runs once, before the loop, so the registration
    // site can gate on a run-latched (sat,signal) verdict instead of a per-epoch
    // >= 2-station count (network/assembly/BiasAdmission.hpp).  It is armed only when both
    // the master flag and the sat-code-bias slice are on; otherwise
    // ctx.admission.active stays false and the legacy per-epoch path runs.
    //
    // The census window is [i0, i0+K) of the synced vector, i0 being the first epoch
    // the loop processes.  The loop start is a lazy time-anchored fast-forward
    // (network.start_offset_hours), not a precomputed index, so that condition is
    // replicated here against the first synced epoch's time.  A warm resume starting a
    // different span reclassifies over that span: the census is per process, not
    // carried in the snapshot, and field-inert for post-processing.
    //
    // Under streaming the front-end replaces the batch epoch source, so this census
    // would latch verdicts from file content the run never processes (stale-day RINEX
    // kept only for station metadata).  A wrong but plausible latched admission would
    // go unnoticed, so the census is refused with a warning and the per-epoch gate is
    // used instead.
    if (ctx.knobs.streaming_on && config_.networkCbAdmission()
        && config_.estimateNetworkSatCodeBias())
        log->warn("Network (RTF-3): network.cb_admission census SKIPPED under "
                  "streaming — it would classify over the metadata RINEX files, "
                  "not the live epochs.  Falling back to the legacy per-epoch "
                  ">= 2-station gate.");
    if (!ctx.knobs.streaming_on && config_.networkCbAdmission()
        && config_.estimateNetworkSatCodeBias()
        && !synced.empty()) {
        std::size_t i0 = 0;
        if (start_offset_s > 0.0) {
            const double anchor_s =
                TimeUtils::continuousGpsSeconds(synced.front().time);
            while (i0 < synced.size()
                   && TimeUtils::continuousGpsSeconds(synced[i0].time) - anchor_s
                          < start_offset_s)
                ++i0;
        }
        ctx.admission = NetworkBiasAdmission::runCensus(
            synced, i0, config_.networkCbAdmissionCensusEpochs(),
            config_.networkCbAdmissionMinStations());
        const auto& adm = ctx.admission;
        ctx.counters.n_cb_admitted =
            static_cast<std::size_t>(adm.n_admitted);
        ctx.counters.n_cb_quarantined_census =
            static_cast<std::size_t>(adm.n_quarantined_census);
        if (adm.active) {
            struct ConsTally { int seen = 0, admitted = 0, lowcount = 0, noifpin = 0; };
            std::map<char, ConsTally> per_cons;
            for (const auto& p : adm.census) {
                ConsTally& t = per_cons[letterOfSat(p.satellite)];   // '?' when unknown
                ++t.seen;
                if (p.verdict == NetworkBiasAdmission::Verdict::Admitted)      ++t.admitted;
                else if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) ++t.lowcount;
                else                                                           ++t.noifpin;
            }
            log->info("Network (CB-ADM): census over {} epochs (K_eff) x {} stations "
                      "[from i0={} of {} synced] — effective_min={}, sustained_floor={}; "
                      "{} (sat,signal) pairs seen, {} admitted, {} quarantined.",
                      adm.census_epochs_used, adm.census_stations, i0, synced.size(),
                      adm.effective_min, adm.sustained_floor,
                      static_cast<int>(adm.census.size()), adm.n_admitted,
                      adm.n_quarantined_census);
            for (const auto& [c, t] : per_cons)
                log->info("Network (CB-ADM):   [{}] {} pairs: {} admitted, {} low-count, "
                          "{} no-IF-pin.", c, t.seen, t.admitted, t.lowcount, t.noifpin);
            constexpr int kMaxRows = 40;
            int shown = 0;
            for (const auto& p : adm.census) {
                if (shown++ >= kMaxRows) {
                    log->info("Network (CB-ADM):   ... ({} more pairs omitted from the table)",
                              static_cast<int>(adm.census.size()) - kMaxRows);
                    break;
                }
                const char* verdict = "ADMIT";
                const char* reason  = "both tiers pass";
                if (p.verdict == NetworkBiasAdmission::Verdict::LowCount) {
                    verdict = "QUAR";
                    reason  = "support < effective_min (tier-1)";
                } else if (p.verdict == NetworkBiasAdmission::Verdict::NoIfPin) {
                    verdict = "QUAR";
                    reason  = "no IF-pinnable pair, < 2 distinct bands (tier-2)";
                }
                log->info("Network (CB-ADM):   {:<11} support={:<3} {:<5} ({})",
                          p.key, p.support, verdict, reason);
            }
        } else {
            log->warn("Network (CB-ADM): census produced NO classification (empty window "
                      "at i0={} of {} synced) — falling back to the legacy per-epoch "
                      ">= 2-station gate.", i0, synced.size());
        }
    }
}

} // namespace GnssProcessor
