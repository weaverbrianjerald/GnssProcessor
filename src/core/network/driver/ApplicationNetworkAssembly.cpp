/**
 * @file ApplicationNetworkAssembly.cpp
 * @brief Assembly-phase member functions of Application::runNetwork: the
 *        pre-loop column-split and iono-row helpers, PASS-A geometry, the
 *        observability prefilter, the shared-column registration, and PASS-B
 *        produce/consume.
 *
 * The bodies originate in ApplicationNetwork.cpp and were moved here by the
 * network decomposition (docs/network_decomposition_plan.md, INC-7).  Former
 * run-TU [&] lambdas are private Application member functions; their ambient
 * captures are explicit parameters or member reads (the capture-to-parameter map
 * is recorded in the INC-7 cut spec).  The include block mirrors
 * ApplicationNetwork.cpp (INC-1 precedent); unused-include cleanup is deferred to
 * INC-12.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "network/emit/EmitRebase.hpp"   // emit-layer frame re-base
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic (M4)
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume (RTB-7)
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts (RTF-1)
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (RTF-1)
#include "network/solve/Estimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "network/solve/Sbasis.hpp"
#include "network/assembly/ExtraBandScope.hpp"  // the extra-band / primary-pair predicates
#include "network/assembly/SecondCodeScope.hpp" // secondCodeCellEdges, the two cell edges
#include "network/assembly/SecondPhaseScope.hpp" // the cell edges + the witness
#include "network/solve/PivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "network/carry/CarryVce.hpp"        // LS-VCE carry-row variance-component monitor (M2)
#include "network/carry/InvariantCarryDriver.hpp"  // DD cycle-basis invariant carry
#include "network/ar/SdEwlAr.hpp"               // the shared EWL band table (sdEwlBands) + increment-2a decision (SD EWL)
#include "network/carry/Coupling.hpp"       // conditional coupling-row builder
#include "network/assembly/ParamInventory.hpp" // the parameter inventory and the receiver IFB rule
#include "network/assembly/Contributions.hpp" // row tags and the contribution tallies
#include "network/qc/PrefitCensus.hpp"   // structural fuse (SPEC_prefit_census_observability, INC-2)
#include "network/diag/ShDevVtec.hpp"     // networkShDevCarryAge
#include "network/assembly/StationBuild.hpp"  // networkAssemblyTally
#include "network/solve/SolveStations.hpp" // the consume and the in-solve track (PB1a)
#include "network/solve/ChainDatum.hpp"    // networkEq34PinAudit
#include "network/ar/ArSuspect.hpp"      // arSuspectNote: the AR search's suspect-corner stamps
#include "filter/StateKeySchema.hpp"
#include "network/driver/RunTypes.hpp"      // run-loop record types: NetStation / StationAssembly / carry PODs (INC-2)
#include "network/qc/RobustStats.hpp"   // median/MAD robust attribution helpers (INC-2)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "network/diag/RunTraces.hpp" // trace-stream ownership + open logic + roster + FlagRate (INC-3)
#include "network/state/RunContext.hpp"    // ctx: knobs + Tier-1 carry state + service counters (INC-4)
#include "network/driver/SetupResult.hpp"   // setup-phase handoff (setup) + navAvailabilityGpsSeconds (INC-5)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, Increment 2)
#include "slip/TurboEditResolver.hpp"   // per-signal MW+GF integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "network/slip/PassASlipScan.hpp"   // the PASS A scan stages
#include "network/slip/PassASlipDrains.hpp" // the PASS A response drain stages
#include "network/slip/SlipQc.hpp"       // slip-QC pure helpers: ifElevMap / parseIfPairResetKey (INC-2)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // identity reads (systemOfSat, ObservableId, ...), carriers
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic (B5)
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to 1 thread inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <random>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>             // std::iota (netar ILS-cap subset ordering)
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun

namespace GnssProcessor {

/**
 * @brief Ends an epoch that PASS B cannot solve: no shared column was registered, or the
 *        consume left none.
 *
 * Counts the epoch as skipped, writes its row of the network trace (week, time of week,
 * present stations, clock satellites, phase-bias columns, four zeros, pins) and a `sync_skip`
 * epoch-status row, and stamps the epoch so the next one's gap test sees it.
 *
 * @param traces           The run's trace streams (the network trace and the epoch status).
 * @param se               The epoch.
 * @param n_skipped        [in/out] The run's skipped-epoch count.
 * @param prev_week        [in/out] Week of the last stamped epoch.
 * @param prev_tow         [in/out] Time of week of the last stamped epoch.
 * @param n_present        Present stations (trace column and status row).
 * @param n_clock          Clock satellites (status row).
 * @param trace_n_clock    The trace row's clock-satellite column; zero where the stage has not
 *                         counted them yet.
 * @param trace_n_pb       The trace row's phase-bias column; zero as above.
 * @param trace_total_pins The trace row's pin column; zero as above.
 * @return EpochAction::SkipEpoch.
 */
static EpochAction networkSkipPassBEpoch(NetworkRunTraces& traces, const SyncedEpoch& se,
                                         std::size_t& n_skipped, int& prev_week, double& prev_tow,
                                         int n_present, int n_clock, int trace_n_clock,
                                         int trace_n_pb, int trace_total_pins) {
    ++n_skipped;
    auto& trace = traces.trace;
    if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                     << ',' << n_present << ',' << trace_n_clock << ',' << trace_n_pb
                     << ',' << 0 << ",0,0,0,0," << trace_total_pins << '\n';
    { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
      traces.emitEpochStatus(se.time, "sync_skip", es); }
    prev_week = se.time.week; prev_tow = se.time.tow;
    return EpochAction::SkipEpoch;
}

EpochAction Application::networkPassAHead_(NetworkRun& run, const NetworkEpoch& ep,
                                           EpochPassA& pass_a) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    std::vector<bool>& present = pass_a.present;  // written
    std::set<std::string>& shared_phase_keys = pass_a.shared_phase_keys;  // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic): the body reads the former runNetwork()
    // locals under their original names, and the former epoch-loop `continue`s are
    // the EpochAction::SkipEpoch returns (their site-specific bookkeeping is kept
    // as it was).
    auto& stations = setup.stations;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& trace                = traces.trace;
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };
    // CB-ADM phase co-gate feed: the run-latched admitted code-key set (null unless
    // the flag is armed and the census ran).  It is passed into every
    // ensureNetworkSharedStates call, so a signal whose code was quarantined also
    // allocates no phase-bias state, and shared_phase_keys (built from the states)
    // excludes it consistently.  Null: byte-identical.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    // Legacy single-pivot datum: the pivot is modelled first, so its usable phase
    // signals restrict the shared phase-bias columns to those with a pivot-receiver
    // ambiguity pin, and an absent pivot skips the epoch (no datum).  Prioritized
    // mode treats the pivot as an ordinary station (modelled in the tail loop from
    // i_lo = 0, with no special skip) and elects the per-(sat,signal) anchors after
    // the cross-station prune, so this block is skipped and a missing pivot does not
    // cost the whole epoch (the pivot is not a single point of failure).
    if (!phase_datum_prioritized) {
        NetStation& piv = stations.front();
        auto it = se.by_station.find(piv.name);
        if (it == se.by_station.end() || it->second.empty()) {
            // No datum station this epoch, so no clean phase-bias gauge: skip.
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.pstate.epoch_time = se.time;
        // The pivot's own elevation-mask / no-model writer.
        // Reached only on the legacy (non-prioritized) datum, where station 0 is
        // modelled here instead of in the parallel tail; both working bases run the
        // prioritized datum, so this block is inert there and the tail below covers
        // the whole population.
        std::map<std::string, DropReason> piv_drops;
        if (!piv.model->computeModels(it->second, piv.pstate, modeled.front(),
                                      ctx.reasons.armed ? &piv_drops : nullptr)
            || modeled.front().empty()) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ",0,0,0,0,0,0,0,0,0\n";
            emitEpochStatus("sync_skip", {});   // only week/tow available
            return EpochAction::SkipEpoch;
        }
        piv.mm->ensureNetworkSharedStates(modeled.front(), {}, cbadm_admit);  // allow all pivot phase signals (minus CB-ADM quarantined)
        present.front() = true;
        for (const auto& [sat, why] : piv_drops) {
            if (why == DropReason::None) continue;
            arcReasonExpand(ctx.reasons, ctx.arc_ledger, 0, sat, "",
                            why == DropReason::BelowMask ? kArcReasonMask
                                                         : kArcReasonNoModel);
        }
    }

    if (!phase_datum_prioritized)
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    return EpochAction::Proceed;
}

void Application::networkPassASlipScan_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                        NetworkSlipFunnel& funnel, EpochEvents& events) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    double dt = ep.clock.dt;
    double pl_now_s = ep.clock.pl_now_s;
    bool carry_warm = ep.clock.carry_warm;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    std::vector<bool>& present = pass_a.present;  // written
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = funnel.carry_slipped_raw;  // written
    SlipQcChannel& qc = run.engines.slip_qc;
    EpochEvents::Slip& slip_ev = events.slip;   // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    auto log = log_manager_.getLogger("main_log");
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    const bool parallel_on = setup.parallel_on;
    const int n_threads = setup.n_threads;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    // The producer AR search's suspect-corner mask is armed (the MW admission
    // records its unknown-integer identities only then).
    const bool ar_suspect_armed = ctx.knobs.ar_suspect_epochs > 0;
    net::ArcTable& arc_rows = ctx.entities.arcs;   // the leveling carry
    auto& fr_if_slip = ctx.counters.fr_if_slip;
    // CB-ADM phase co-gate feed (see networkPassAHead_): null unless the flag is
    // armed and the census ran.  Withholds a quarantined signal's phase-bias state.
    const std::set<std::string>* cbadm_admit =
        (config_.estimateNetworkSatCodeBias() && ctx.admission.active)
            ? &ctx.admission.admitted : nullptr;

    const int i_lo = phase_datum_prioritized ? 0 : 1;

    // PASS-A slip-scan seam (INC-6): the per-station buffers that the parallel loop
    // below fills and the serial slip-QC drains (network/slip/SlipQc.cpp) consume.
    PassABuffers pa(stations.size());
    // The elevation-mask and no-model writers.  The refusal
    // is taken inside computeSatelliteGeometry, which carries no station identity,
    // so it is captured through the model's own per-satellite drop-reason map and
    // folded here, where the station index is known.  One map per station index:
    // the loop below is OpenMP-parallel, so a shared buffer would race (the same
    // reason present[] is folded through a per-index char buffer).  Allocated only
    // while the writers are armed, so an unarmed run pays one bool test.
    const bool sid_screens = ctx.reasons.armed;
    std::vector<std::map<std::string, DropReason>> pa_drops(
        sid_screens ? stations.size() : 0);

    // The handles the stages read, built once per epoch.
    const PassAView v{se, ctx, traces, config_, log, dt, pl_now_s, carry_warm, stations,
        modeled, present, shared_phase_keys, carry_slipped_ambs, qc, slip_ev, n_sta_total,
        parallel_on, n_threads, phase_datum_prioritized, carry_invariant, trace_flag_rates,
        ar_suspect_armed, arc_rows, fr_if_slip, cbadm_admit, i_lo, pa, sid_screens, pa_drops};

    // The stages, in epoch order. Each binds the names it uses from the view.
    passAScanStations(v);       // models, shared states, MW and IF-phase detection (parallel)
    passAFoldPresent(v);        // present[] and the thrown-station log, serial
    passAFoldDropReasons(v);    // elevation-mask and no-model reasons into the arc ledger
    passASecondCode(v);         // second code channel cells, edges and counters
    passASecondPhase(v);        // second phase channel cells, edges, counters and witness
    passARawObservables(v);     // shared raw slip observables, qualification, CS-GF authority
    const bool datum_detect_on = !phase_datum_prioritized
                                 && config_.networkCycleSlipDatumStationDetect()
                                 && config_.networkCycleSlipMwDetect();
    passADatumStationMwDetect(v, datum_detect_on);
    passAFoldMwAdmission(v);    // MW admission census into the run counters

    // Serial slip-QC drains (INC-6), implemented in network/slip/SlipQc.cpp: the
    // phase-leveling T0 repair drain, the MW-detect / LLI refloat and the IF-phase
    // TD driver, in that order.
    networkSlipDrainT0Repairs(pa, config_, stations, present, i_lo, n_sta_total,
                              arc_rows, se, log);

    // Per-epoch re-level response state (SPEC_slip_relevel_walk §6): cleared here
    // (the harvest and the PASS-B inject consumed last epoch's sets after this point
    // in the previous epoch), filled by the two drains below, and read by the inject
    // site, the harvest guard and the IF-TD bridge.  nullptr when the knob is off,
    // so the drains' inserts are never reached (byte-identical; the
    // refsat_mark_sats pattern).
    RelevelEpochState* relevel_ptr =
        config_.networkCycleSlipRelevelFreeEpoch() ? &slip_ev.relevel : nullptr;

    bool mw_carry_warm = carry_warm;
    passAResumeSlipWarmup(v, mw_carry_warm);
    CommonBandEpoch common_band;
    passACommonBandVerdict(v, common_band);
    const CommonBandEpoch* common_band_ptr = common_band.armed ? &common_band : nullptr;

    std::map<net::ArcTuple, RelevelPend> relevel_pend =
        passAMwRefloatStage(v, mw_carry_warm, datum_detect_on, relevel_ptr, common_band_ptr);
    passAIfTdStage(v, relevel_ptr, relevel_pend, common_band_ptr);
    passAGfArcJumpStage(v, relevel_ptr, relevel_pend, common_band_ptr);
    passAWindupRestartStage(v, relevel_ptr, relevel_pend);
    passAGapReentryStage(v, relevel_ptr, relevel_pend);
    passAInjectedFreeStage(v, relevel_ptr, relevel_pend);
    net::assignRelevelPend(ctx.entities, relevel_pend);
}

void Application::networkAssemblyPrefilter_(NetworkRun& run, const NetworkEpoch& ep,
                                            EpochPassA& pass_a) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    const SlipQcChannel& qc = run.engines.slip_qc;
    std::vector<std::vector<PppObservation>>& modeled = pass_a.modeled;  // written
    const std::vector<bool>& present = ep.pass_a.present;
    int& n_dropped_sats = pass_a.n_dropped_sats;  // written
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    auto& stations = setup.stations;
    const int n_sta_total = setup.n_sta_total;
    auto& slip_raw_trace       = traces.slip_raw_trace;

    // Raw cycle-slip observables (network.trace_slip_raw), written whether or not any
    // slip detector is armed.  Written before the observability pre-filter, so a
    // satellite about to be dropped is still captured.  For each present
    // station+satellite the primary dual-frequency pair (the two lowest distinct bands
    // whose phase and matching code "C"+sig[1:] are present) yields MW, GF (=L1−L2) and
    // first-signal phase-minus-code (all in m), plus the per-band RINEX LLI.  Absolute
    // values: the slip signature comes from differencing adjacent epochs offline.
    //
    // The arithmetic lives in network/slip/RawObservables.cpp (slip-QC hardening, spec
    // §1): the LLI-corroboration and gap-coincident consumers need the same numbers as evidence, and a single definition of
    // "the raw step" keeps the three consumers from diverging.  This site is a pure
    // consumer walking qc.epoch.rows in the same order, so its rows are byte-unchanged
    // when the trace is the only consumer.
    if (slip_raw_trace.is_open() && qc.epoch.computed) {
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            for (const auto& r : qc.epoch.rows[static_cast<std::size_t>(i)]) {
                slip_raw_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << stations[i].name << ',' << r.satellite << ','
                    << r.sig1 << ',' << r.sig2 << ','
                    << fmt::format("{:.2f}", r.elev_deg) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.mw_m) : std::string{}) << ','
                    << (r.dual ? fmt::format("{:.4f}", r.gf_m) : std::string{}) << ','
                    << fmt::format("{:.4f}", r.l1_c1_m) << ',' << r.lli1 << ',' << r.lli2 << '\n';
            }
        }
    }

    // --- Cross-station observability pre-filter -------------------------
    // A satellite clock is well determined for a network product only when at
    // least kMinStationsPerSat stations observe it: a single-station satellite
    // clock is tied to that one receiver clock and, at marginal geometry, becomes
    // the extra near-zero direction that trips the rank gate.  Such satellites are
    // dropped from the network entirely for this epoch, both the shared
    // clock/phase-bias states and the underlying observations.  The observations
    // must go too: a satellite kept in the design with no shared clock column would
    // pin its station's receiver clock absolutely and so remove the clock gauge the
    // datum is meant to pin (declared > observed, and the gate would then reject
    // every epoch).  Dropping the whole satellite keeps one weak satellite from
    // forcing the whole epoch to be withheld; the gate remains the final safety net
    // for a real residual deficiency.
    constexpr int kMinStationsPerSat = 2;
    n_dropped_sats = 0;
    {
        std::map<std::string, int> sat_station_count;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates())
                ++sat_station_count[StateKeys::satelliteFromSatClockKey(kv.first)];
        }
        std::set<std::string> dropped_sats;
        for (const auto& kv : sat_station_count)
            if (kv.second < kMinStationsPerSat) { dropped_sats.insert(kv.first); ++n_dropped_sats; }

        if (!dropped_sats.empty()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                // The cross-station observability gate is a coverage gate too:
                // the same rule as the phase-bias gate one level
                // up, applied to the satellite clock.  It is the one that removes
                // rows, so it opens admitted gaps where the phase-bias gate opens
                // none.  Both share the `gate` bit; the drop-edge counters keep them
                // separable at the readout.
                for (const std::string& ds : dropped_sats)
                    arcReasonExpand(ctx.reasons, ctx.arc_ledger,
                                    static_cast<int>(i), ds, "", kArcReasonGate);
                // Remove the dropped satellites' observations so their rows are
                // not built (and so cannot pin the receiver clock).
                auto& obs = modeled[i];
                obs.erase(std::remove_if(obs.begin(), obs.end(),
                              [&](const PppObservation& o) { return dropped_sats.count(o.satellite) != 0; }),
                          obs.end());
                // Prune the now-orphaned shared states.
                StateManager& sm = *stations[i].sm;
                std::set<std::string> keep_clk, keep_pb;
                for (const auto& kv : sm.getSatClockStates())
                    if (!dropped_sats.count(StateKeys::satelliteFromSatClockKey(kv.first)))
                        keep_clk.insert(kv.first);
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (p && !dropped_sats.count(p->satellite)) keep_pb.insert(kv.first);
                }
                sm.pruneSatClockStatesExcept(keep_clk);
                sm.pruneSatPhaseBiasStatesExcept(keep_pb);
            }
        }
    }
}

EpochAction Application::networkAssemblyRegisterColumns_(NetworkRun& run, const NetworkEpoch& ep,
                                                         EpochColumns& columns) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkSetupResult& setup = run.setup;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    const std::vector<bool>& present = ep.pass_a.present;
    const std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    std::vector<std::string>& registered_clock_sats = columns.registered_clock_sats;  // written
    std::vector<std::pair<std::string, std::string>>& registered_code_keys = columns.registered_code_keys;  // written
    std::vector<std::string>& col_desc = columns.col_desc;  // written
    int& n_clock = columns.n_clock;  // written
    int& n_pb = columns.n_pb;  // written
    int& n_cb = columns.n_cb;  // written
    int& n_present = columns.n_present;  // written
    const EpochEvents& events = ep.events;
    // ---- End of the bindings ----
    // Alias block (INC-7 migration mechanic).
    const auto& nc = config_.getNetworkConfig();
    auto& stations = setup.stations;
    auto& sh_model = setup.sh_model;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& last_n_present = ctx.counters.last_n_present;
    auto& last_n_clock_sats = ctx.counters.last_n_clock_sats;
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;

    // --- Register the shared column layout (reset the hoisted estimator) ---
    est.reset();
    std::set<std::string> clk_seen;
    for (size_t i = 0; i < stations.size(); ++i) {
        if (!present[i]) continue;
        for (const auto& kv : stations[i].sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            est.registerClock(sat);
            if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
        }
    }
    // CB-ADM per-epoch registration decision (on only): an admitted code pair is
    // registered only when >= 2 present stations hold its state this epoch (the
    // per-epoch guard).  A single-station code column is confounded with that
    // receiver's clock and goes near-null; registering on window-latched admission
    // alone (at >= 1 station) brings those columns back (22 -> 43 near-null
    // directions on the 305-station arm).  The anti-flap property is kept because
    // the admitted set stays latched (census): only the per-epoch registration
    // follows coverage, and the SM state is never pruned, so a withheld column
    // re-registers as soon as >= 2 stations return.  cbadm_reg_code_keys holds the
    // (sat,signal) code columns registered this epoch.
    //
    // The phase co-gate is not applied here.  It sits upstream, at
    // phase-bias state allocation in MeasurementModel::ensureNetworkSharedStates,
    // and it keys off the run-latched census-admitted set, not off
    // cbadm_reg_code_keys, the narrower per-epoch registered subset.  Two separate
    // reasons, both load-bearing:
    //  - Level: withholding a registerPhaseBias for a key still present in
    //    shared_phase_keys breaks the "every shared_phase_key is a registered
    //    column" invariant that eq34 / carry / snapshot rely on (a registration-site
    //    co-gate crashes the on path).  Allocating no phase-bias state at all keeps
    //    shared_phase_keys consistent everywhere by construction.
    //  - Granularity (a deliberate ruling): the co-gate stays at census
    //    granularity.  Following per-epoch code coverage would churn phase-state
    //    allocation every epoch, which the anti-flap design exists to prevent, and
    //    thrash eq34/carry/snapshot.  The accepted residue is a transient orphan
    //    window, counted by ctx.counters.n_cbadm_phase_orphan_key_epochs below.
    //
    // Why a sparse signal must leave the shared system on both bias families: that
    // phase column has no same-signal code co-anchor and, with two stations, no
    // reachable ambiguity pin, so it collapses to a pure-PB near-null gauge (89 such
    // directions on the 305-station arm, the phase counterparts L2X/L7X/L1I of the
    // quarantined code pairs C2X/C7X/C1I).  Withholding it folds the bias into each
    // station's float ambiguity, symmetric with the code sponge.  A per-satellite
    // co-gate is too coarse (a satellite admitted on its dense signals would keep
    // its sparse signals' phase columns); the granularity must match the code
    // admission unit, which is per-(sat,signal).
    std::set<std::string> cbadm_reg_code_keys;   // raw "sat|sig" code keys registered this epoch (admitted and >=2 present stations)
    const bool cbadm_on =
        config_.estimateNetworkSatCodeBias() && ctx.admission.active;
    if (cbadm_on) {
        std::map<std::string, int> code_epoch_count;   // admitted "sat|sig" -> #present stations this epoch
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatBiasStates())
                if (ctx.admission.isAdmitted(kv.first)) ++code_epoch_count[kv.first];
        }
        for (const auto& [key, cnt] : code_epoch_count)
            if (cnt >= 2) cbadm_reg_code_keys.insert(key);
    }
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Register every shared_phase_key unconditionally: the CB-ADM phase
        // co-gate sits upstream at phase-bias state allocation
        // (MeasurementModel::ensureNetworkSharedStates, cbadm_admitted_code_keys),
        // so shared_phase_keys already excludes a quarantined signal's phase.
        // Gating here instead would break the "every shared_phase_key is a
        // registered column" invariant that eq34/carry/snapshot rely on (crash).
        est.registerPhaseBias(parts->satellite, parts->signal);
        // Measure the granularity seam's orphan window.  This loop is the
        // only place that sees all three sets at once: the census-admitted set
        // (ctx.admission), this epoch's registered code keys (cbadm_reg_code_keys)
        // and the shared phase set (shared_phase_keys).  An admitted code key that
        // fell below 2 present stations this epoch has its code column withheld
        // while its same-signal phase column stays shared: one (key, epoch) pair of
        // orphan window.  Expected 0 at 305-station density (every admitted
        // satellite has >= 2 present stations there); a sparse-tier run showing it
        // nonzero and coincident with withholds is the data that would reopen the
        // ruling.
        if (cbadm_on) {
            // Key-format asymmetry: phase-bias keys carry the phase observable
            // ("C26|L2X|PB"), while the admission census and cbadm_reg_code_keys
            // carry the code one ("C26|C2X").  The companion mapping is
            // L<band><attr> -> C<band><attr>, the same transform the state-alloc
            // co-gate applies (MeasurementModel::ensureNetworkSharedStates).
            // Reusing the phase signal unchanged would match nothing and leave this
            // counter permanently 0, inert wiring that reads as "no orphans"
            // (standing invariant #4).
            const auto phase =
                SignalId::fromObsCode(systemOfSat(parts->satellite), parts->signal);
            if (phase) {
                const std::string code_key =
                    StateKeys::makeSatBiasKey(parts->satellite, phase->obsCode(ObsType::Code));
                if (ctx.admission.isAdmitted(code_key) && !cbadm_reg_code_keys.count(code_key))
                    ++ctx.counters.n_cbadm_phase_orphan_key_epochs;
            }
        }
    }
    // Register the satellite code-bias (OSB) columns (W6, full multi-band): every
    // (sat,signal), i.e. every frequency band and every tracking channel
    // (attribute), for which at least two present stations carry an OSB state.
    // Collected (deduplicated, first-seen order) for the code-bias datum
    // constraints, the D2 carry-forward and the solution trace.  Empty unless
    // estimate_sat_code_bias.
    //
    // The default admission gate (CB-ADM off) is per-epoch >=2-station coverage: a
    // single-station code bias is fully confounded with that station's receiver
    // code bias and cannot be separated, so it stays a local nuisance (its sat-bias
    // state is pruned below, and its raw code row folds into the receiver clock /
    // slant iono, as without W6).  Everything with >=2 stations is a first-class
    // shared OSB column and is datumed below: the GIM between-satellite iono
    // structure makes the inter-band DCBs observable, while same-band channels
    // (C1C vs C1W, C2W vs C2L, ...) need no iono at all (at equal frequency the
    // receiver and ionosphere terms cancel in the raw between-channel difference,
    // leaving the satellite inter-channel bias).  The datum pins every registered
    // column, including those on a third band.
    //
    // Under CB-ADM (ctx.admission.active, network.cb_admission on), registration
    // follows the run-latched census verdict and the per-epoch coverage: an
    // admitted pair registers when >= 2 present stations hold its state this epoch
    // (cbadm_reg_code_keys above), and its state is kept through coverage dips, so
    // it re-registers when coverage returns (the anti-flap property).  A
    // quarantined pair is never pruned (station_keep always holds it), so it
    // survives as a station-local sponge (Schur-eliminated station-side, never
    // registered, never datumed).  See network/assembly/BiasAdmission.hpp.
    if (config_.estimateNetworkSatCodeBias()) {
        std::set<std::string> cb_seen;
        NetworkBiasAdmission& adm = ctx.admission;
        if (!adm.active) {
            // --- CB-ADM off: legacy per-epoch >=2-station gate (byte-identical) ---
            std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatBiasStates())
                    ++cb_station_count[kv.first];
            }
            std::set<std::string> keep_keys;
            for (const auto& [key, cnt] : cb_station_count)
                if (cnt >= 2) keep_keys.insert(key);

            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    if (!keep_keys.count(kv.first)) continue;
                    station_keep.insert(kv.first);
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;
                    est.registerCodeBias(parts->satellite, parts->signal);
                    if (cb_seen.insert(kv.first).second)
                        registered_code_keys.emplace_back(parts->satellite, parts->signal);
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        } else {
            // --- CB-ADM on: run-latched admission, per-epoch >=2 registration and
            // station-local quarantine.  cbadm_reg_code_keys (computed above the
            // phase loop) is the admitted set restricted to keys with >= 2 present
            // stations this epoch; only those register a shared column.  Every SM
            // state is kept (station_keep holds it unconditionally: quarantined and
            // per-epoch-withheld admitted states are never pruned), so a withheld
            // column takes the scatter's codeBiasColumn-throw local fallback this
            // epoch and re-registers when coverage returns.
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    station_keep.insert(kv.first);   // never pruned while the flag is on
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;            // malformed key: kept (harmless), never registered
                    if (cbadm_reg_code_keys.count(kv.first)) {
                        est.registerCodeBias(parts->satellite, parts->signal);
                        if (cb_seen.insert(kv.first).second)
                            registered_code_keys.emplace_back(parts->satellite, parts->signal);
                    } else if (!adm.isAdmitted(kv.first)
                               && !adm.seen.count(kv.first)
                               && adm.late_tracked.insert(kv.first).second) {
                        // A key the census never classified was first seen after the
                        // window and is implicitly quarantined (invariant #4
                        // telemetry).  An admitted key that is only below the
                        // per-epoch coverage this epoch is not late: it is kept and
                        // re-registers later.
                        ++ctx.counters.n_cb_late_quarantined;
                    }
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        }
    }

    // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
    // layout is fixed (all (N+1)² coefficients of every shell, every epoch,
    // independent of the satellite set), so the field is continuous across epochs
    // and the Kaula prior can keep every coefficient soft-pinned (the rank gate then
    // stays at #constellations regardless of coverage; design §6.1).  Dual layer
    // (Iono-2): the second shell registers under its own "SH2" keys after the
    // legacy layer, preserving the layer-0 column order.
    if (config_.estimateNetworkIonoSh() && sh_model) {
        for (const ShLayer& lay :
             shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                      setup.sh_model2.get()))
            for (const auto& c : lay.model->coeffs())
                est.registerVtecCoeff(c.n, c.m, c.trig, lay.layer);
    }

    // Clock clones (network.carry_coupling_clock_clone): the one-epoch clock clones, after every
    // other registration so that no existing column index moves (the clones are the last
    // columns), and before the label map and beginEpoch.  No-op off.
    networkClockCloneRegister(ctx, est, registered_clock_sats, events);
    // Shared history (network.carry_coupling_shared_history): the shared-history columns, after
    // the clones (filter/NetworkSharedHistory).  No-op off.
    networkSharedHistoryRegister(ctx, est);

    n_clock = static_cast<int>(registered_clock_sats.size());
    n_pb    = static_cast<int>(shared_phase_keys.size());
    n_cb    = static_cast<int>(registered_code_keys.size());
    n_present = static_cast<int>(std::count(present.begin(), present.end(), true));
    last_n_present    = n_present;   // most-recent station-presence count for the health heartbeat (RTB-6)
    last_n_clock_sats = n_clock;     // most-recent registered-clock-sat count for the health heartbeat (RTB-6, P4b)

    // Column -> human descriptor map (W6 estimability diagnostic): lets the one-shot
    // spectrum dump name the shared parameters that span the near-zero
    // eigen-directions (unpinned gauges).  Populated only when the estimability
    // trace (off by default) is armed; it feeds nothing but that dump, so a
    // production run skips the string-building entirely.
    col_desc.resize(static_cast<size_t>(est.numShared()));
    if (spectrum_diag_budget > 0 && nc.trace_estimability) {
        for (const std::string& sat : registered_clock_sats)
            col_desc[static_cast<size_t>(est.clockColumn(sat))] = "CLK|" + sat;
        for (const auto& pk : shared_phase_keys) {
            const auto p = StateKeys::parseSatPhaseBiasKey(pk);
            if (p) col_desc[static_cast<size_t>(est.phaseBiasColumn(p->satellite, p->signal))]
                       = "PB|" + p->satellite + "|" + p->signal;
        }
        for (const auto& kv : registered_code_keys)
            col_desc[static_cast<size_t>(est.codeBiasColumn(kv.first, kv.second))]
                = "CB|" + kv.first + "|" + kv.second;
        // Name the SH-VTEC columns (required, design §6.4): otherwise a missing SH
        // datum prints as an anonymous colNNN in the spectrum dump,
        // indistinguishable from a clock-datum fault.
        if (config_.estimateNetworkIonoSh() && sh_model) {
            for (const ShLayer& lay :
                 shLayers(config_.getGlobalVtecShConfig(), sh_model.get(),
                          setup.sh_model2.get()))
                for (const auto& c : lay.model->coeffs())
                    col_desc[static_cast<size_t>(
                        est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer))] =
                        fmt::format("{}|{}|{}|{}", lay.layer > 0 ? "SH2" : "SH",
                                    c.n, c.m, c.trig);
        }
        for (const auto& [sat, col] : ctx.carry.ccl.col)   // clock clones; empty off
            col_desc[static_cast<size_t>(col)] = "CLKC|" + sat;
        for (std::size_t j = 0; j < ctx.carry.shx.col.size(); ++j)   // shared history; empty off
            if (ctx.carry.shx.col[j] >= 0)
                col_desc[static_cast<size_t>(ctx.carry.shx.col[j])] = "HIST|" + std::to_string(j);
    }

    if (est.numShared() == 0)
        return networkSkipPassBEpoch(traces, se, n_skipped, prev_week, prev_tow,
                                     n_present, n_clock, /*trace_n_clock=*/0, /*trace_n_pb=*/0,
                                     /*trace_total_pins=*/0);

    est.beginEpoch();
    return EpochAction::Proceed;
}

EpochAction Application::networkPassB_(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a,
                                       EpochAssembly& assembly) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    const SyncedEpoch& se = ep.se;
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    NetworkEstimator& est = run.engines.est;
    int n_clock = ep.columns.n_clock;
    int n_pb = ep.columns.n_pb;
    int n_present = ep.columns.n_present;
    std::vector<StationAssembly>& asm_res = assembly.asm_res;  // written
    std::vector<int>& est_index_of = assembly.est_index_of;  // written
    int& total_pins = assembly.total_pins;  // written
    double& sec_asm_ms_epoch = assembly.sec_asm_ms_epoch;  // written
    double& sec_asm_wall_ms = run.timing.asm_wall_ms;
    // ---- End of the bindings ----
    const bool sec_timers_on = ctx.knobs.sec_timers_on;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;

    // --- PASS B: per-station assembly, then serial scatter -------------
    // Split into a parallel produce phase (pure per-station compute into
    // pre-sized slots, no shared write) and a serial consume phase that scatters
    // each block into the shared arrowhead in deterministic station order.
    // computeMeasurementUpdate, splitStation and the pivot pin fold touch only
    // station i's own state and const queries on `est` (its column layout is
    // frozen after registration; addStation is the only mutation and runs
    // serially below), so the produced blocks and their add order are identical
    // for any thread count.
    //
    // The assembly wall-clock timer (D0) brackets the parallel produce
    // (per-station row build) and the serial scatter (est.addStation): the
    // "assembly" section.
    std::chrono::steady_clock::time_point sec_asm_t0;
    if (sec_timers_on) sec_asm_t0 = std::chrono::steady_clock::now();

    // The serial stages before the parallel produce.
    networkShDevCarryAge(run, ep);
    networkCouplingReinitHistory(run, ep);

    // The parallel produce: one station's block per OpenMP iteration, the station steps of
    // kStationBuildSteps (network/assembly/StationBuild.hpp).
    networkAssemblyProduce(run, ep, gim_, assembly);

    // The serial stages after the parallel produce.
    networkAssemblyTally(run, ep, assembly);
    networkPrefitFuseEpoch(run, ep, assembly);
    const bool epoch_ok = networkAssemblyConsume(run, ep, pass_a, assembly);
    networkSolveStationsTrack(run, ep, assembly, epoch_ok);
    networkEq34PinAudit(run, ep, assembly, epoch_ok);

    // Close the assembly wall-clock timer (D0; produce + scatter).  Accumulated even
    // when the epoch skips below, because the assembly work was still spent.
    if (sec_timers_on) {
        sec_asm_ms_epoch = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sec_asm_t0).count();
        sec_asm_wall_ms += sec_asm_ms_epoch;
    }

    // The station side of "contributions as armed" (the rows and diagonal
    // entries of the stations the estimator took, per kind), and every assembled station's
    // tags checked against its rows.  Serial, trace class; the epoch tail adds the shared
    // folds and writes the table.
    networkContributionsStationDrain(ctx.contributions, asm_res, est_index_of,
                                     se.time.week, se.time.tow);

    if (!epoch_ok || est.numShared() == 0)
        return networkSkipPassBEpoch(traces, se, n_skipped, prev_week, prev_tow,
                                     n_present, n_clock, n_clock, n_pb, total_pins);

    return EpochAction::Proceed;
}

} // namespace GnssProcessor
