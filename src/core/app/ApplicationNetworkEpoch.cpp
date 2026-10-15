/**
 * @file ApplicationNetworkEpoch.cpp
 * @brief Application::networkProcessEpoch_: one synced epoch through the PPP-RTK network
 *        producer (card NOM3 step EP1).
 *
 * The body is runNetwork's former per-epoch lambda (processSyncedEpoch), moved four columns
 * left by scripts/nom_ep1_cut.py and checked line for line by scripts/nom_ep1_verify.py
 * (check V1a).  What the move changes, each a declared transform of the EP1 specification
 * (section 5.3): the epoch's locals live in a NetworkEpoch (app/NetworkEpoch.hpp) built right
 * after the admission; the run's objects are reached through a NetworkRun
 * (app/NetworkRun.hpp); the D0b brackets are networkSubBegin / networkSubEnd; a converted
 * seam takes (run, ep[, kept...][, outputs...]) and binds its former parameters in its own
 * preamble.  The text below the preamble is not edited by hand: a change is a change of the
 * frozen source or of the canonical table (scripts/nom_ep1/rows_*.py), regenerated with
 * `python scripts/nom_ep1_cut.py --shared-only`.
 *
 * Do not add code here; CLAUDE.md's decision tree names the seam each concern belongs in.
 */

#include "app/Application.hpp"
#include "app/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "app/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // emit-layer frame re-base
#include "products/NetworkClockEmitRebase.hpp" // clock emission re-base + per-sat step marks
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (card 5, RTF-1)
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkClockLevel.hpp"         // satellite-clock level monitor and restore raise (card CTU)
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "filter/NetworkCarryVce.hpp"        // LS-VCE variance-component monitor for carry rows (M2)
#include "filter/NetworkPriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (card 4, M1 C3b)
#include "filter/StateKeySchema.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat, rinexLetter, systemName
#include "filter/NetworkRunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "filter/NetworkWlRunTypes.hpp" // RAII and section-timer types hoisted out of runNetwork() (INC-9b)
#include "filter/NetworkRunGroups.hpp"  // the driver-owned run groups: engines, emit engines, timing, closed-loop tally (card NOM TD1a)
#include "filter/NetworkRobustStats.hpp"   // median / MAD helpers for robust attribution (INC-2)
#include "filter/NetworkPhaseDatum.hpp"    // census, anchor election, drop-and-heal (INC-8b)
#include "filter/NetworkAmbStateTrace.hpp" // per-arc carry-state trace: drain and erase capture (RF-1 §7.4)
#include "filter/NetworkAmbFloat.hpp"      // floored birth prior and bounded drop (RF-1 step 4)
#include "filter/NetworkHandoverStransform.hpp" // kStsExactTolM (RF-2b: the declaration step test)
#include "filter/NetworkChainDatum.hpp"    // eq34 single-receiver-chain datum (card 3, INC-8b)
#include "filter/NetworkAnchorHygiene.hpp" // eq34 reattach hygiene: anchor-host eligibility + pin bridge
#include "filter/NetworkSignalIntegrity.hpp" // flicker/gap: band-state classifier + unvetted phase demotion
#include "filter/NetworkMeasurementAdmission.hpp" // product-referenced admission: the corrupt-channel screen
#include "filter/NetworkGrossLos.hpp"    // gross line-of-sight screen (card GL, §24.18)
#include "filter/NetworkRaim.hpp"        // post-fit network RAIM (INC-9)
#include "filter/NetworkClockQc.hpp"     // post-solve satellite-clock QC (INC-10)
#include "filter/NetworkFrameDiag.hpp"   // frame-guard trigger instrumentation (P1-3/P1-4, SPEC_day129_frame_guards)
#include "filter/NetworkStationBlock.hpp"  // per-station local-block audit, measure-only (card ZP, §24.10)
#include "filter/NetworkRhsAttrib.hpp"   // reduced-RHS class attribution (era-2 injector probe)
#include "filter/NetworkMdb.hpp"               // Baarda reliability numbers / MDB (SPEC_redundancy_census)
#include "filter/NetworkRedundancyCensus.hpp"  // redundancy / testability census (SPEC_redundancy_census)
#include "filter/NetworkPrefitCensus.hpp"      // prefit census / fuse / birth check / topology (SPEC_prefit_census_observability)
#include "filter/NetworkDatumGraph.hpp"        // structural S-basis dimension census (SPEC_datum_graph_census)
#include "filter/NetworkSolPrep.hpp"    // emit-side solution prep and discontinuity signalling (INC-11pre)
#include "filter/NetworkEmitStore.hpp"    // emit, store, product write and closed loop (INC-11)
#include "filter/NetworkTeardown.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"  // phase-visible consistency instrument (card PVN)
#include "filter/NetworkPbHoldClone.hpp"       // the smoothed hold's clock clones (card SMH)
#include "filter/NetworkCarryDisposition.hpp"  // the decided-once fold disposition (card PRM phase 1b)
#include "filter/NetworkProducerAr.hpp" // log-only producer AR and NEQ export (INC-9b)
#include "filter/NetworkFixedCycles.hpp" // the fixed-cycle fold and its ledger of fixed cycles: a detached held solve only the emission reads (card FCF)
#include "filter/NetworkInject.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp" // canonical-pair admission seam (card CPA)
#include "app/SignalRoster.hpp"  // signal roster and arc-ledger observatory (card SID phase 1)
#include "filter/NetworkSecondPhaseScope.hpp"  // second phase channel, epoch tail (card SID phase 6a)
#include "filter/NetworkChannelAll.hpp"      // all-channel census (card SID phase 6b)
#include "filter/NetworkLedgerEmit.hpp"  // emitter read against the ledger, declared roster (card SID phase 5a)
#include "filter/NetworkInstallCarry.hpp" // installPosterior and the WL zone-4 fold / inject (INC-9b)
#include "filter/NetworkDdPriorBuild.hpp" // buildCarryDdPriors free function (INC-9b)
#include "filter/NetworkSolveGlue.hpp" // datum, solve and within-epoch RAIM (INC-9b)
#include "filter/NetworkIonoDiag.hpp"    // iono / ZWD / anchor-info viz traces (INC-11b)
#include "filter/NetworkDdFrac.hpp"    // cross-station DD fractional-residual trace (INC-11b)
#include "filter/NetworkSdEwlAr.hpp"   // SD EWL Route-B integer decision (increment 2a)
#include "filter/NetworkShDevVtec.hpp"    // SH-deviation carry KF and VTEC-coefficient trace (INC-11b)
#include "filter/NetworkShHealth.hpp"    // SH-field health probe, carry LS-VCE (INC-11b)
#include "filter/NetworkLevelAuthority.hpp"  // per-epoch level-authority census (LV-6)
#include "filter/NetworkLeveling.hpp"    // phase-leveling carry harvest, D2/C1 stash (INC-11b)
#include "filter/NetworkStashStatus.hpp"    // carry re-stash, LS-VCE fold, epoch-status QC row (INC-11b)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "logging/NetworkRunTraces.hpp" // trace-stream ownership, open logic, roster, FlagRate (INC-3)
#include "app/NetworkRunContext.hpp"    // knobs, tier-1 carry state and service counters, as ctx (INC-4)
#include "app/NetworkSetupResult.hpp"   // setup-phase handoff (setup) and navAvailabilityGpsSeconds (INC-5)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // pure slip-QC helpers: ifElevMap, parseIfPairResetKey (INC-2)
#include "slip/NetworkSlipOracle.hpp"   // simulation-only oracle slip response (level-preserving ceiling)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // the slip-QC wavelength provider (GLONASS slots)
#include "realtime/RealTimeEngine.hpp"
#include "realtime/NetworkStreamFrontEnd.hpp"  // multi-station streaming epoch source (RTF-3)
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (Eigen runs single-threaded inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic)
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

namespace GnssProcessor {

// === One synced epoch through the full producer (RTF-3 carve-out) =========
// The per-epoch body as a callable over `se` alone; every other name it reads is
// loop-invariant setup or ctx-carried run state, captured by reference.  Loop
// control maps one to one (a top-level `continue` becomes return Next, the
// max-duration `break` becomes return Stop), so the batch iteration below is
// structurally identical to an inline loop.  The callable exists so a streaming
// epoch source (network.streaming, the RTF-3 multi-station front-end) can drive the
// identical per-epoch body without a pre-built synced vector.
// Card NOM3 step EP1: the callable is Application::networkProcessEpoch_, over the run
// (app/NetworkRun.hpp) and the synced epoch.
Application::LoopSignal Application::networkProcessEpoch_(NetworkRun& run,
                                                          const SyncedEpoch& se_raw) {
    // ---- The run under the names the epoch's text still reads (card NOM3 step EP1; generated by
    //      scripts/nom_ep1_cut.py; each line the frozen runNetwork declaration with the run's
    //      objects re-pointed). Step EP2 removes these lines with the blocks that read them. ----
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const NetworkConfig& nc = run.nc;
    NetworkSetupResult& setup = run.setup;
    const bool brdc_rts = setup.brdc_rts;
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    auto& stations = setup.stations;
    const int n_threads = setup.n_threads;
    const int seam_threads = setup.seam_threads;
    const bool carry_par = setup.carry_par;
    NetworkProductEmitter& emitter = *setup.emitter;
    ClosedLoopTally& cl_tally = run.closed_loop;
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& anchor_incumbent = ctx.carry.anchor_incumbent;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& start_anchor_s = ctx.carry.start_anchor_s;
    auto& local_carry = ctx.carry.local_carry;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    NetworkCarryVce& carry_vce_est = run.engines.carry_vce;
    NetworkPriorResidual& prires_est = run.engines.prires;
    const bool   ura_on        = ctx.knobs.ura_on;
    const bool   raim_on       = ctx.knobs.raim_on;
    const double start_offset_s = ctx.knobs.start_offset_s;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_gated_epochs = ctx.counters.n_gated_epochs;
    auto& n_gated_key_epochs = ctx.counters.n_gated_key_epochs;
    auto& max_dur_anchor_s = ctx.counters.max_dur_anchor_s;
    auto& max_dur_anchored = ctx.counters.max_dur_anchored;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    const bool carry_invariant         = ctx.knobs.carry_invariant;
    const bool carry_innov_gate = ctx.knobs.carry_innov_gate;
    NetworkInvariantCarryDriver& carry_driver = run.engines.carry_driver;
    NetworkEmitEngines& emit_engines = run.emit;
    auto& carry_sat_vertex = ctx.carry.carry_sat_vertex;
    auto& carry_group_id = ctx.carry.carry_group_id;
    auto& carry_prev_tow = ctx.carry.carry_prev_tow;
    auto& carry_innov_pending = ctx.carry.carry_innov_pending;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    NetworkRunTiming& timing = run.timing;
    auto& n_loop_epochs = ctx.counters.n_loop_epochs;
    auto& decim_counter = ctx.counters.decim_counter;
    NetworkEstimator& est = run.engines.est;
    std::vector<SubBlockTimer>& sec_sub_ep0 = timing.sec_sub_ep0;
    // ---- End of the run's names ----

    // Optional testing fast-forward (network.start_offset_hours): epochs are skipped
    // until the elapsed data span from the first synced epoch reaches the offset, and
    // the solve cold-starts there.  Skipped epochs are not counted, modeled, solved or
    // stashed, so the carry begins empty at the offset.  This reproduces an externally
    // driven late-hour event (for example the ~4.3 h lock-in) after a fresh warm-up,
    // not the exact accumulated history.
    if (start_offset_s > 0.0) {
        const double now_s = TimeUtils::continuousGpsSeconds(se_raw.time);
        if (start_anchor_s < 0.0) start_anchor_s = now_s;
        if (now_s - start_anchor_s < start_offset_s) return LoopSignal::Next;
    }
    // The log prefix is bound before the injector runs, so the injector's own
    // warnings carry this epoch's bracket, not the previous one.  se.time ==
    // se_raw.time always (the injector never touches the time tag), so the
    // position of this call affects log text only.
    LogManager::setCurrentEpochTime(se_raw.time);
    // Event-harness seam (filter/NetworkInject.cpp), the single observation
    // injection site.  It is upstream of computeModels, every T0 detector,
    // networkComputeRawObservables and assembly, so a one-scalar write
    // on the RinexObservation (phase in cycles) moves the metre-valued design row
    // and the cycle-valued MW/GF/IF-TD feed together.  It sits after the
    // start-offset gate so a fast-forwarded epoch cannot consume an event.
    // Unarmed, it returns se_raw itself (no copy, no branch, byte-identical).
    // `se` shadows nothing; the whole body below reads it.
    SyncedEpoch se_inject_scratch;
    const SyncedEpoch& se_admit = networkInjectEpoch(ctx, log, traces, se_raw,
                                                     se_inject_scratch);
    // Canonical-pair admission seam (filter/NetworkCanonicalPairAdmission.cpp,
    // card CPA).  It sits immediately after the injector, on the same contract,
    // because this is the only site upstream of everything the withhold must be
    // absent from: computeModels, the T0 detectors, networkComputeRawObservables,
    // assembly, the RAIM pools and the traces all derive from this
    // RinexObservation.  Dropping the record here is therefore equivalent to a
    // missing RINEX record, which lets the row-level gap re-entry tier read the
    // satellite's return as a re-entry.  It screens the producer roster only, so
    // the held-out closed-loop consumer is untouched.  Disarmed, it returns
    // se_admit itself (no copy, no walk, byte-identical).
    SyncedEpoch se_cpa_scratch;
    const SyncedEpoch& se = networkCanonicalPairAdmission(ctx, log, traces, stations,
                                                          se_admit, se_cpa_scratch);
    // The epoch: everything the stages below hand each other (app/NetworkEpoch.hpp).
    NetworkEpoch ep(se);
    EpochWorkspace& ws = ep.ws;

    // Signal-roster seam (app/SignalRoster.cpp, card SID phase 1).  It sits
    // immediately after the canonical-pair admission returns the epoch, for the
    // reason given above: this site is upstream of computeModels, the T0
    // detectors, networkComputeRawObservables, assembly, the RAIM pools and the
    // traces, so the roster answers "which signal stands for this band at this
    // station" from the same RinexObservation all of them derive from.  It fills
    // ctx.roster and advances ctx.arc_ledger on every epoch (card NOM step N3a).
    // The disagreement census, the "signals as planned" table and the trace read
    // them; the solve reads them only through a SID reader armed by its own knob
    // (the roster tracker, the ledger re-entry response, the anchor hygiene, the
    // ledger emit), so with none armed the solve and the wire are unchanged.
    networkSubBegin(run, SB_ROSTER);
    networkBuildSignalRoster(ctx, config_, log, traces, se, stations);
    networkSubEnd(run, SB_ROSTER);

    // Reduced-RHS class attribution: the estimator's shared-info capture recorder
    // is armed before any prior-inject site runs this epoch (the post-solve seam
    // reads the captures).  One branch per epoch when disarmed.
    //
    // With the flight recorder armed (rhs_attrib_on_trip) the recorder runs on
    // every epoch, because a trip is known only after the solve and a trip must be
    // able to attribute its own epoch.  The cost is ~10 vectors x n_shared doubles,
    // cleared at each beginEpoch, which is negligible next to the solve.
    est.attribArm(ctx.knobs.rec_on
                  || (ctx.knobs.rhs_attrib_tow >= 0.0
                      && se.time.tow >= ctx.knobs.rhs_attrib_tow - 0.25
                      && ctx.carry.rhs_attrib_done < ctx.knobs.rhs_attrib_n_ep));
    // The per-epoch trip handoff is a one-epoch value.  It is cleared here, at the
    // top of the epoch, so a trip raised by a site that runs before the
    // SB_FRAMEDIAG consumer cannot leak into the next epoch's dump.
    ctx.carry.rhs_trip_pending.clear();

    // Datum graph census: the spectral hand-off is also a one-epoch value.  An
    // epoch that skips, withholds or throws before the rank gate would otherwise
    // leave the previous epoch's eigenvalue count in place, and the next census
    // pass would reconcile this epoch's topology against it with no column saying
    // so.  Clearing it here, at the top, means the census only ever reads an
    // observation from its own epoch.
    if (ctx.knobs.dg_on) ctx.carry.dg_spec.clear();

    // Feeds the staged broadcast nav records whose availability (toc, GPS scale)
    // this epoch has reached.  The store's current/previous retention then evolves
    // as under live arrival: IODE rolls happen at their true times (the card 6
    // re-levels fire), and the fit-window guard sees fresh records instead of the
    // end-of-day pair a bulk load would leave.  The first processed epoch catches up
    // over any start_offset span in one sorted batch, which ends in the same store
    // state as epoch-by-epoch feeding.
    if (brdc_rts) {
        auto& eph_store = LiveEphemerisStore::getInstance();
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);
        while (brdc_nav_cursor < brdc_nav_staged.size()
               && navAvailabilityGpsSeconds(brdc_nav_staged[brdc_nav_cursor]) <= now_s) {
            eph_store.insert(brdc_nav_staged[brdc_nav_cursor], /*from_igs=*/true);
            ++brdc_nav_cursor;
        }
    }
    // Card NBP: the native broadcast clock a priori feeds the store by transmission
    // time (healthy records), so the record the wire stamps this epoch is the store's
    // current one when the consumer reads it.  Null in every other mode.
    if (setup.brdc_apriori)
        setup.brdc_apriori->feedStore(LiveEphemerisStore::getInstance(),
                                      TimeUtils::continuousGpsSeconds(se.time));

    // Optional max-duration cutoff (processing.max_duration_s): anchor on the
    // first synced epoch, stop once the elapsed data span reaches the limit.
    if (ctx.knobs.max_duration_s > 0.0) {
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);
        if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
        else if (now_s - max_dur_anchor_s >= ctx.knobs.max_duration_s) {
            log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                      ctx.knobs.max_duration_s);
            return LoopSignal::Stop;
        }
    }
    // Epoch decimation (T2e): only every Nth synced epoch is processed (N=1 ⇒ the
    // modulo is always 0 ⇒ nothing is skipped ⇒ byte-identical).  It sits after the
    // start-offset and max-duration guards, so the physical-time window and break are
    // evaluated on every raw epoch, and before ++n_loop_epochs, so decimated-out
    // epochs are not counted and n_loop_epochs stays the processed-epoch count.  The
    // brdc_rts nav catch-up is idempotent across skips.
    if ((decim_counter++ % ctx.knobs.epoch_decimation_n) != 0) {
        // A decimated-out epoch has fully synced observations but is not processed.
        return LoopSignal::Next;
    }
    ++n_loop_epochs;  // [TIMING]
    // The satellite ids of no modelled constellation, and of SBAS (card NOM N0b), counted
    // on every processed epoch before any model reads its observations.  Measure-only;
    // the count is printed under the parameter table in the run-end tail.
    networkParamInventoryNoteSatellites(ctx.param_inventory, se);

    // Per-epoch section-timer baseline (D0).  Deltas of the estimator's run-cumulative
    // accumulators against this snapshot give this epoch's reduce, solve, cov-inverse
    // and cov-consumer cost; sec_epoch_t0 anchors the epoch wall, and
    // sec_asm_ms_epoch is filled at the assembly wall below.  Inert (no clock read)
    // when the toggle is off.
    NetworkNormalEquations::SectionTimers sec_t0;
    std::chrono::steady_clock::time_point sec_epoch_t0;
    double& sec_asm_ms_epoch = ep.assembly.sec_asm_ms_epoch;
    if (ctx.knobs.sec_timers_on) {
        sec_t0 = est.sectionTimers();
        sec_epoch_t0 = std::chrono::steady_clock::now();
        sec_sub_ep0 = timing.sub_blocks;   // per-epoch deltas of the sub-block run totals (D0b)
    }

    // Rotation of the unbounded service CSVs (P1); the body is in NetworkRunTraces
    // (INC-4).  Off ⇒ a byte-identical no-op.
    traces.rotateServiceCsvs(nc, log);

    // Health heartbeat (RTB-6); the body is in NetworkRunTraces (INC-4), and the
    // window anchors and last_health_write_s live in ctx.counters.
    traces.writeHealthRow(se.time, nc, ctx.counters, timing.loop_begin,
                          ura_on, raim_on, pl_on, ura_sigma0_ewma,
                          last_pivot, grade_metric_s);

    // The event-row writer's body lives in NetworkRunTraces (L1/L2); this thin
    // forwarder adds the epoch time, so the call sites below pass only the row.
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };

    const double dt = (prev_tow < 0.0) ? 0.0
        : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

    // Phase-leveling carry timing (increment 1): the continuous-GPS seconds of this
    // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start warmup
    // has elapsed.  The warmup counts emitted product epochs, not raw loop epochs, so
    // the carry waits for the clock/SH convergence horizon; locking an unconverged
    // ambiguity onto a contaminated cold-start value would propagate that value.
    // n_emitted here excludes the current epoch, which is not yet emitted.
    const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
    const bool   carry_warm = pl_on && (static_cast<int>(n_emitted) >= pl_warmup);
    ep.clock = EpochClock{ dt, pl_now_s, carry_warm };

    // This epoch's slip event feed for the DD invariant carry: the (app station index,
    // ambiguity key) pairs the PASS-A slip detectors flagged.  It is filled at the
    // detection site (the slip/NetworkSlipQc.cpp drains) rather than read from
    // pending-reseed at fold time, because PASS-B MeasurementModel::compute re-seeds
    // and clears the pending-reseed flag before buildCarryDdPriors runs (the flag is a
    // one-shot the assembly consumes).  It is recorded regardless of carry_warm (the
    // detectors run cold, and the DD carry is live from epoch 1), so the carry is
    // slip-protected through the whole warmup and not only after the SH field's
    // convergence gate releases.  carry_invariant only.
    std::set<std::pair<int, std::string>>& carry_slipped_ambs = ep.funnel.carry_slipped_ambs;
    // The innovation gate's flags from the previous epoch seed this epoch's slip set (a
    // fine slip caught after last epoch's solve re-taps its arc fresh here, via the same
    // C3c membership-exclusion path the coarse detectors feed).  One-shot: consumed now.
    if (carry_innov_gate && !carry_innov_pending.empty()) {
        carry_slipped_ambs.insert(carry_innov_pending.begin(), carry_innov_pending.end());
        carry_innov_pending.clear();
    }
    // Retap-only drain (card 4 D5 v4, network.carry_invariant_innov_retap_only): the
    // corroborated-flat subset of those flags is drained in lockstep.  It is a strict
    // subset of what just went into the funnel (the entries stay in the funnel and
    // every side consumer is untouched), and it names the arcs whose raw MW/GF level
    // was measured and did not step, that is, the arcs with no integer jump for a
    // refloat to complete.  networkBuildCarryDdPriors spares exactly those from the
    // A_old membership exclusion, so a DD the gate re-tapped in-epoch keeps its history
    // instead of being reborn from a diffuse prior every epoch.  Cleared
    // unconditionally so a disarmed run cannot inherit a stale set.
    ctx.carry.carry_innov_flat_now.clear();
    if (!ctx.carry.carry_innov_pending_flat.empty()) {
        ctx.carry.carry_innov_flat_now.swap(ctx.carry.carry_innov_pending_flat);
        ctx.carry.carry_innov_pending_flat.clear();
    }
    // Erase-policy split (Iono-2 v3k, SPEC_iono2_mechB_v3 addendum): the raw-detector
    // portion of the funnel, captured at insertion.  PASS A fills this set and the
    // driver unions it into the funnel below, so every existing funnel consumer sees
    // the same union, while the shdev pre-pass can tell raw-confirmed slips (erase)
    // from D5-only re-taps.  97 % of D5-only re-taps have no raw observable step: they
    // are iono-tension events the deviation should absorb rather than be erased by.
    std::set<std::pair<int, std::string>>& carry_slipped_raw = ep.funnel.carry_slipped_raw;

    // --- PASS A: per-station geometry + shared-state layout -------------
    ep.pass_a.modeled = std::vector<std::vector<PppObservation>>(stations.size());
    std::vector<std::vector<PppObservation>>& modeled = ep.pass_a.modeled;
    ep.pass_a.present = std::vector<bool>(stations.size(), false);
    std::vector<bool>& present = ep.pass_a.present;
    // Legacy: the pivot's allocated phase-bias keys are the shared set.
    // Prioritized: seeded empty here; the anchor election after the cross-station
    // observability prune rebuilds it as the anchored union over present stations
    // (each key backed by exactly one elected pin).
    std::set<std::string>& shared_phase_keys = ep.pass_a.shared_phase_keys;
    // The pivot-first head, the PASS-A parallel tail and the slip scans live in
    // ApplicationNetworkAssembly.cpp.  networkPassAHead_ models the legacy pivot
    // (with its skip bookkeeping; SkipEpoch ends the epoch) and seeds the legacy
    // shared set; the SB_SLIP wall times the scan.
    if (networkPassAHead_(se, ctx, setup, traces, modeled, present,
                          shared_phase_keys) == EpochAction::SkipEpoch)
        return LoopSignal::Next;

    networkSubBegin(run, SB_SLIP);   // block 1: PASS-A slip scans, pre-assembly (D0b)
    ++slip_qc.epoch_idx;   // monotonic processed-epoch index (card G gap arithmetic)
    networkPassASlipScan_(se, ctx, setup, traces, dt, pl_now_s, carry_warm,
                          modeled, present, shared_phase_keys,
                          carry_slipped_raw, slip_qc);
    // The union preserves the funnel's previous contents: the PASS-A drains in
    // NetworkSlipQc.cpp only insert, so routing their writes through
    // carry_slipped_raw changes nothing downstream.
    carry_slipped_ambs.insert(carry_slipped_raw.begin(), carry_slipped_raw.end());
    networkSubEnd(run, SB_SLIP);       // close block 1, slip scans (D0b)
    networkSubBegin(run, SB_PREASM);   // pre-assembly measurement build, post-slip → assembly wall (D0b)

    // The raw slip observables and the cross-station observability pre-filter live in
    // ApplicationNetworkAssembly.cpp; the writer-before-prune ordering contract is
    // structural inside networkAssemblyPrefilter_.
    int& n_dropped_sats = ep.pass_a.n_dropped_sats;
    networkAssemblyPrefilter_(se, ctx, setup, traces, slip_qc,
                              modeled, present, n_dropped_sats);

    // Flicker / gap signal integrity.  Classifies every present (station, satellite)
    // line of sight against its own band history, publishes the per-station phase
    // sigma-scale map that PASS B's fold reads, and refloats a line of sight whose
    // band flickered back with no producer-registered gap.
    //
    // The call has one valid position.  It must run after the PASS-A slip drains and
    // their union, because it inserts into carry_slipped_ambs / _raw and every
    // consumer must see those insertions; after networkAssemblyPrefilter_, so the
    // census counts only the LOS set that folds rows and a satellite erased by the
    // two-station observability gate is not reported as an event; and before the
    // phase-datum census and election, because a withhold prunes phase-bias states
    // the election might otherwise seat a datum pin on.
    //
    // It is not placed in the PASS-B serial pre-pass, which runs after the election.
    // The epoch unit is slip_qc.epoch_idx, the counter the gap instrument uses, so
    // their re-entry totals are directly comparable.  No-op unless
    // network.signal_integrity.census.
    networkSignalIntegrity(ctx, config_, log, traces, se.time, slip_qc.epoch_idx,
                           stations, present, modeled,
                           carry_slipped_ambs, carry_slipped_raw,
                           carry_invariant, carry_warm);
    // Product-referenced measurement admission, the first layer of the defence in
    // depth (this screen, then the prefit fuse).  It is the absolute test for a
    // channel whose code and phase are displaced together, the class every
    // differential check passes by construction
    // (stations have been seen +198.2 km and +99.9 km off SP3+CLK).  The statistic is
    // `observed - modeled` on code alone, centred on the station median; `modeled` is
    // the state-free deterministic model, which lets the screen fire at a cell's
    // birth, where a prefit innovation structurally cannot.
    //
    // The call has one valid position.  After the PASS-A slip drains and their union,
    // because it inserts into carry_slipped_ambs / _raw for its onset, heal
    // (readmission) and station-guard refloats.  After networkAssemblyPrefilter_, so
    // a satellite erased by the observability gate cannot enter the median, raise a
    // verdict, or count as admitted.  After networkSignalIntegrity, because `modeled`
    // is rebuilt every epoch, and screening first would make the flicker classifier
    // read each withhold as a band loss and each heal as a FlickerRegain, creating
    // the events that census exists to measure (this seam only reads `observed` /
    // `modeled`, which the classifier does not write, so the later position costs
    // nothing).  And before the phase-datum election and
    // networkAssemblyRegisterColumns_, because a withhold prunes orphaned phase-bias /
    // code-bias / clock states the election could otherwise pin, and would otherwise
    // leave the ≥ 2-station code-bias coverage gate counting a station that feeds it
    // nothing.  No-op unless network.adm_screen or network.adm_trace.
    networkMeasurementAdmission(ctx, config_, log, traces, se.time,
                                slip_qc.epoch_idx, stations, present, modeled,
                                carry_slipped_ambs, carry_slipped_raw,
                                carry_invariant, carry_warm);
    // The final shared phase-bias set, and which station anchors (gauge-pins) each
    // (sat,signal).  Two realizations of the same minimum constraint (exactly one
    // ambiguity pin per phase-bias freedom):
    //   Legacy (network.phase_datum_prioritized off): the single pivot's pruned
    //     phase-bias states are the shared set (each backed by a pivot-receiver
    //     pin).  Byte-identical.
    //   Prioritized: per (sat,signal), the highest-coverage receiver that tracks it
    //     with a live ambiguity is elected as its anchor.  The anchor is decoupled
    //     from the clock pivot, so a satellite low or unseen at the clock pivot keeps
    //     a clean gauge at another station instead of jumping.
    if (!phase_datum_prioritized) {
        shared_phase_keys.clear();
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    } else {
        // This epoch's candidate census and the network.phase_datum_min_stations
        // coverage gate (filter/NetworkPhaseDatum, INC-8b).  It is built once here and
        // shared by whichever realization the dispatch below picks.  The builder also
        // prunes the phase-bias states of gated keys, which all three elections
        // require to have happened already, and it is the sole writer of the two
        // run-end gate counters (computing the census twice would double-count them).
        PhaseDatumCandidates cnd;
        AnchorHygiene hyg;   // eq34 reattach hygiene verdicts (inactive when off)
        networkBuildPhaseDatumCandidates(config_, stations, present, modeled,
                                         cnd, n_gated_epochs, n_gated_key_epochs);

        // eq34 reattach hygiene.  Ages this epoch's anchorable arcs, decides which
        // keys may be re-hosted and on what, runs the INC-2 bridge lifecycle, and
        // (when a key has no converged host and no bridge) removes it from the
        // candidate union as the coverage gate removes a thin key.  It must sit here:
        // after the census (it mutates cnd.union_keys and needs the coverage prune
        // done) and after the PASS-A slip drains (carry_slipped_ambs must be complete,
        // or a just-refloated arc reads as mature; invariant #1), but before any
        // election reads the union.  No-op unless a hygiene knob is armed.
        networkAnchorHygiene(ctx, config_, log, traces, se.time, stations, present,
                             carry_slipped_ambs, cnd, hyg);

        if (phase_datum_eq34) {
            // Chain datum (card 3).  It mutates the carries before the solve and
            // fills ctx.carry.eq34_reref, so it must stay ahead of the D3c RPB
            // allocation below (it is the sole writer of shared_phase_keys on this
            // path, and D3c derives its need_rpb set from that) and therefore ahead
            // of est.beginEpoch and PASS B.  The full ordering contract is in
            // filter/NetworkChainDatum.hpp.
            networkElectChainDatum(ctx, config_, log, stations,
                                   present, cnd, hyg, carry_slipped_ambs,
                                   shared_phase_keys);
        } else {
            // Two-edge continuous handover.  The ledger is passed only when armed
            // (it decides the pin target, so it affects the solve), but the event
            // sink is unconditional (since R0): the cold re-pin of the default
            // posture is also an event on both edges, and leaving it unlogged is
            // §5.07.4 defect 5.  The rows are trace-class (network_events.csv under
            // trace_events); the solve is untouched, and `tally.reattaches` counts
            // either way.
            std::vector<AnchorHandoverEvent> handovers;
            const bool hv_on = ctx.knobs.pd_handover_continuous;
            // The re-referenced (sat,signal) set (queue item 1(d)): filled here when
            // armed, consumed by the D2/C1 carry fold (marginalize) and the erase
            // directly below, and cleared by the carry stash, not by this election.
            // A withheld epoch skips the stash, so its prev_carry still holds the
            // retired-frame values while the incumbent swap has already persisted;
            // clearing here would let that stale carry fold unmarked one epoch
            // later.  Clearing at the stash keeps the mark until a solve commits the
            // new frame.
            auto& pa_reref = ctx.carry.phase_anchor_reref;
            const std::size_t pa_before = pa_reref.size();
            // Anchor-slip handover (network.cycle_slip.anchor_slip_handover,
            // docs/SPEC_event_harness.md §12.7).  It must sit exactly here: after
            // every PASS-A slip drain and the signal-integrity / admission refloats
            // (so both freed sets are complete), and immediately before the
            // election, its only consumer.  It stages the same one-epoch candidacy
            // ban the harness injector uses, so a family whose anchor arc was just
            // freed is handed to a successor, at the successor's own carried
            // value_abs, in this epoch: before the freed arc's level is re-drawn
            // from one epoch of code and the whole family's gauge follows it.  Knob
            // off: not called, nothing staged, and the `inject` argument below
            // resolves unchanged.
            if (ctx.knobs.anc_slip_handover) {
                const int n_st = networkStageAnchorSlipHandovers(
                        stations, anchor_incumbent,
                        ctx.carry.relevel.free_ambs, carry_slipped_ambs,
                        ctx.inject);
                if (n_st > 0) {
                    ctx.counters.n_anc_slip_staged += n_st;
                    ++ctx.counters.n_anc_slip_epochs;
                }
            }
            // Cold anchor fallback (SPEC_event_harness §15): the candidacy pass's
            // read-only view.  It is built here and not inside the election because
            // every member is a run-scope carry object: the freed sets this epoch's
            // drains just filled, and the leveling carry the age bar reads
            // `birth_ep` from.  Default-constructed when the knob is off, which
            // reproduces the election without the card.
            AnchorCandidacyUsable cand_usable;
            if (ctx.knobs.pd_candidacy_usable) {
                cand_usable.on           = true;
                cand_usable.satellite_clause = ctx.knobs.pd_candidacy_satellite;
                cand_usable.min_age_ep   = ctx.knobs.pd_candidacy_min_age_ep;
                cand_usable.now_ep       =
                    static_cast<long long>(ctx.counters.n_loop_epochs);
                cand_usable.free_ambs    = &ctx.carry.relevel.free_ambs;
                cand_usable.slipped_ambs = &carry_slipped_ambs;
                cand_usable.local_carry  = &ctx.carry.local_carry;
            }
            // The phase datum as a held S-basis (RF-2,
            // docs/refound/RF-1_steps345_mechanism_2026-09-15.md).  Assembled here
            // because this scope owns the ledger, the carry, the flag map and this
            // epoch's elevations; `on` is false on every unarmed run, and the
            // pointer below is then null.
            PhaseHeldBasis held_basis;
            std::vector<std::map<std::string, double>> pd_elev_of;
            held_basis.on = ctx.knobs.pd_held_basis;
            if (held_basis.on) {
                held_basis.min_age_ep    = ctx.knobs.pa_min_age_ep;
                held_basis.flag_quiet_ep = ctx.knobs.pa_flag_quiet_ep;
                held_basis.now_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
                held_basis.basis   = &ctx.carry.phase_basis;
                held_basis.flag_ep = &ctx.carry.amb_flag_ep;
                held_basis.carry   = &ctx.carry.local_carry;
                // This epoch's LOS elevations, the election's rank key (item 3).
                pd_elev_of.assign(modeled.size(), {});
                for (std::size_t mi = 0; mi < modeled.size(); ++mi)
                    for (const auto& mo : modeled[mi])
                        pd_elev_of[mi][mo.satellite] = mo.elevation;
                held_basis.elev = &pd_elev_of;
            }
            // The coverage gate's drop edge (card EV7 §6).  It is passed on every
            // run, armed or not: requirement F2 asks the unarmed run to report the
            // size of the class, and detection is pure observation over the census
            // and the pre-swap incumbent map (the erase below is the only thing the
            // knob gates).
            PhaseGateDrop gate_drop;
            gate_drop.on = ctx.knobs.pd_gate_drop_erase;
            // The ledger read at the election's first-sighting branch (card SID
            // phase 4, S2 / B1).  `observe` alone is the observatory and is
            // byte-identical; `restore` adds the phase 4b action.  Both are false on
            // every unarmed run, which leaves every branch on the pointer
            // unreachable inside the election.  ctx.roster.active is true on every
            // network run (networkSignalRosterArm sets it before the loop and nothing
            // clears it); it is kept as the other SID readers keep it.
            AnchorReentry pd_reentry;
            pd_reentry.observe = ctx.knobs.ledger_screens && ctx.roster.active;
            pd_reentry.restore = pd_reentry.observe && ctx.knobs.ledger_anchor;
            pd_reentry.led     = &ctx.arc_ledger;
            pd_reentry.state   = &ctx.carry.sid_anchor_reentry;
            pd_reentry.now_ep  = ctx.arc_ledger.epochIndex();
            networkElectPhaseAnchors(ctx.knobs.pd_sticky, stations, present, cnd, hyg,
                                     ctx.counters.hyg,
                                     anchor_incumbent,
                                     ctx.carry.anchor_pin_target,
                                     this_epoch_anchor,
                                     shared_phase_keys,
                                     hv_on ? &ctx.carry.handover : nullptr,
                                     &handovers,
                                     ctx.knobs.pd_handover_max_var,
                                     ctx.knobs.pa_carry_reref ? &pa_reref
                                                              : nullptr,
                                     ctx.knobs.pd_handover_max_var_ratio,
                                     // This epoch's forced retirements, or
                                     // nullptr (the normal path).  Two sources:
                                     // the event-harness schedule and the
                                     // production anchor-slip handover staged
                                     // just above; either one alone makes the
                                     // directive set live.
                                     (ctx.inject.armed
                                      && !ctx.inject.anchor_now.empty())
                                     || !ctx.inject.anchor_now_slip.empty()
                                         ? &ctx.inject : nullptr,
                                     &cand_usable,
                                     // The held S-basis (RF-2).  Null unless armed,
                                     // so every branch it gates is unreachable and
                                     // the election is byte-identical.
                                     held_basis.on ? &held_basis : nullptr,
                                     // always non-null (card EV7 §6, see above)
                                     &gate_drop,
                                     // In effect null when neither half is armed:
                                     // the election tests `observe` on its first
                                     // line (card SID phase 4).
                                     &pd_reentry);
            // Both edges of the gate are events (card EV7 §6, F2).  One row per drop
            // edge, with the identities on either side (what left, which station was
            // anchoring it, which stations still carry it) and `handled` saying
            // whether the response ran, so an unarmed row and an armed row are
            // separable offline.  Trace-class; the solve never reads these.
            for (const auto& gd : gate_drop.events) {
                ++ctx.counters.n_pd_gate_drop_edges;
                if (gd.handled) ++ctx.counters.n_pd_gate_drop_handled;
                // Asks the arc ledger what state the dropped family's arcs are in,
                // counted where the edge is already evented (card SID phase 1,
                // class (h)).  Read-only and a no-op when the knob is off.
                networkSignalRosterGateDrop(ctx, gd.sat, gd.signal);
                // The coverage-gate writer (card SID phase 4a, S1).  The gate
                // removes a phase-bias column and, when armed, the family's carried
                // ambiguities; it does not remove the phase rows, which is the
                // whole of card EV7's mechanism, so this bit is expected to open
                // few or no admitted gaps.  It is recorded anyway, so the readout
                // can state that the gate fired and opened nothing.
                arcReasonExpand(ctx.reasons, ctx.arc_ledger, -1, gd.sat,
                                gd.signal, kArcReasonGate);
                emitEvent("PHASE_ANCHOR", "gate_drop",
                          StateKeys::makeAmbiguityKey(gd.sat, gd.signal),
                          gd.survivors.empty() ? std::string("-") : gd.survivors,
                          static_cast<double>(gd.n_trackers),
                          static_cast<double>(ctx.knobs.pd_min_stations),
                          gd.handled ? "erase" : "none",
                          fmt::format("survivors={};trackers_now={};prev_anchor={};"
                                      "handled={}",
                                      gd.survivors.empty() ? std::string("-")
                                                           : gd.survivors,
                                      gd.n_trackers,
                                      gd.prev_anchor.empty() ? std::string("-")
                                                             : gd.prev_anchor,
                                      gd.handled ? 1 : 0));
            }
            // Folds the ledger read (card SID phase 4, S2 / B1).  It happens here,
            // where every other election readback is folded, so the counters and
            // the two event tiers are written in one place and in epoch order.
            networkSignalRosterAnchorRead(ctx, traces, se.time, pd_reentry);
            // The families the gate-drop response owns, for the erase-cause split
            // below.  A gated key is erased from union_keys before the election's
            // loop, so it cannot also be a handover or first-sighting mark this
            // epoch; the two populations are disjoint by construction.
            std::set<std::pair<std::string, std::string>> gate_drop_fams;
            for (const auto& gd : gate_drop.events)
                if (gd.handled) gate_drop_fams.insert({gd.sat, gd.signal});
            // Folds the election's write-back into the run counters (RF-2).
            if (held_basis.on) {
                auto& HC = ctx.counters;
                HC.n_pb_basis_held      += held_basis.n_held;
                HC.n_pb_basis_elected   += held_basis.n_elected;
                HC.n_pb_basis_cold      += held_basis.n_elected_cold;
                HC.n_pb_basis_refuse_young   += held_basis.n_refused_young;
                HC.n_pb_basis_refuse_flagged += held_basis.n_refused_flagged;
                HC.n_pb_basis_rank_changed   += held_basis.n_rank_changed;
                HC.n_pb_basis_refuse_unusable += held_basis.n_refused_unusable;
                HC.n_pb_basis_elev_fallback   += held_basis.n_elev_fallback;
                HC.n_pb_basis_depose_flag     += held_basis.n_depose_flag;
                HC.n_pb_basis_depose_death    += held_basis.n_depose_arc_death;
                HC.n_pb_basis_depose_unusable += held_basis.n_depose_unusable;
                HC.n_pb_basis_value_drift    += held_basis.n_value_drift;
            }
            // Split-referenced satellite census (§14.5).  Always on and
            // knob-independent (invariant #4): the number of satellites whose
            // per-frequency phase-bias families are anchored at different
            // stations, whose arc levels walk independently.  It is read here
            // because the election has just swapped this epoch's anchors into
            // anchor_incumbent, so the map is current.  Pure and O(map), with no
            // contact with the solve: the counters below are its only output, so
            // an unarmed run measures the same thing and the effect of the scope
            // on datum fragmentation is comparable across arms.
            {
                const SplitRefCount sr =
                    networkSplitReferencedSats(anchor_incumbent);
                ctx.counters.n_cb_split_sats_sum += sr.split;
                ctx.counters.n_cb_split_multi_sum += sr.multi_anchored;
                ++ctx.counters.n_cb_split_epochs;
                if (sr.split > ctx.counters.n_cb_split_sats_max)
                    ctx.counters.n_cb_split_sats_max = sr.split;
            }
            // Event-harness readback: the election filled in the directive with the
            // stations it retired and elected.  One row per directive, and the
            // firing ledger is closed here: an anchor_handover that named a
            // satellite no family used never fires, and the run-end readout
            // reports that.
            for (const auto& d : ctx.inject.anchor_now) {
                if (d.n_families == 0) continue;
                std::string ret, ele;
                for (const auto& r : d.retired) ret += (ret.empty() ? "" : "|") + r;
                for (const auto& e2 : d.elected) ele += (ele.empty() ? "" : "|") + e2;
                traces.emitEvent(se.time, "INJECT", "anchor_handover", d.sat, "",
                                 static_cast<double>(d.n_families), std::nullopt,
                                 "injected",
                                 fmt::format("mode={};families={};forced_cold={};"
                                             "retired={};elected={}",
                                             d.cold ? "cold" : "continuous",
                                             d.n_families, d.n_forced_cold,
                                             ret.empty() ? "-" : ret,
                                             ele.empty() ? "-" : ele));
                log->warn("Network (fault inject): TOW {:.1f} anchor_handover on {} "
                          "({}) forced {} family(ies): retired [{}] -> elected [{}].",
                          se.time.tow, d.sat, d.cold ? "cold" : "continuous",
                          d.n_families, ret.empty() ? "-" : ret,
                          ele.empty() ? "-" : ele);
                for (std::size_t k = 0; k < ctx.inject.events.size(); ++k)
                    if (ctx.inject.events[k].kind == "anchor_handover"
                        && ctx.inject.events[k].sat == d.sat
                        && !ctx.inject.onset_logged[k]) {
                        ++ctx.inject.n_applied[k];
                        ctx.inject.onset_logged[k] = true;
                        ++ctx.inject.fired_by_kind["anchor_handover"];
                    }
            }
            // Anchor-slip handover readback (§12.7), both edges with identities
            // (invariant #7): the retired incumbent is the row's station; the
            // successor and the branch are its detail.  A cold outcome is a gauge
            // re-realization, and the row says so; a directive that found no
            // successor left the family unpinned for the epoch, and the row says
            // that too.
            for (const auto& d : ctx.inject.anchor_now_slip) {
                if (d.n_families == 0) {
                    // The family was not in this epoch's union (coverage-gated,
                    // bridged or pruned).  There are no identities to report; it is
                    // counted so the run-end readout can separate it from a
                    // handover that happened.
                    ++ctx.counters.n_anc_slip_inert;
                    continue;
                }
                const char* branch = d.n_no_successor > 0 ? "none"
                                   : (d.n_cold_fallback > 0 ? "cold" : "continuous");
                if (d.n_no_successor > 0) {
                    ++ctx.counters.n_anc_slip_no_successor;   // not re-hosted
                } else {
                    ++ctx.counters.n_anc_slip_handed;
                    if (d.n_cold_fallback > 0) ++ctx.counters.n_anc_slip_cold;
                    else                       ++ctx.counters.n_anc_slip_continuous;
                }
                emitEvent("PHASE_ANCHOR", "anchor_slip_handover",
                          StateKeys::makeAmbiguityKey(d.sat, d.signal),
                          d.retired.empty() ? std::string("-") : d.retired.front(),
                          std::nullopt, std::nullopt, "handover",
                          fmt::format("to={};branch={}{}",
                                      d.elected.empty() ? std::string("-")
                                                        : d.elected.front(),
                                      branch,
                                      // Cold anchor fallback (§15): what the
                                      // candidacy pass skipped to get here, so
                                      // the cold-handover census can be re-run
                                      // from the trace.  Absent with the knob off.
                                      ctx.knobs.pd_candidacy_usable
                                          ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                        d.n_cand_skipped,
                                                        d.cand_first_unusable.empty()
                                                            ? std::string("-")
                                                            : d.cand_first_unusable)
                                          : std::string{}));
            }
            // Both edges in one row (invariant #7): which station released the
            // gauge, which took it, and whether the frame was preserved.  A
            // handover logged only on its incoming half is the unlogged
            // re-referencing that the FT63 topology ledger was built to recover.
            for (const auto& h : handovers)
                traces.emitEvent(se.time, "PBGAUGE",
                                 h.continuous ? "handover_continuous"
                                              : "handover_cold",
                                 h.amb, h.to, h.target_m, std::nullopt,
                                 h.continuous ? "s_transform" : "re_realize",
                                 "from=" + (h.from.empty() ? std::string("(none)")
                                                           : h.from)
                                 // With the ratio ceiling armed, the successor's ledger
                                 // var and first-stash var are added to the row (the
                                 // prerequisite named in SPEC_amb_iono §7 item 15: trace
                                 // the successor's conditional var per handover, not
                                 // only the target).
                                 + (ctx.knobs.pd_handover_max_var_ratio > 0.0
                                        ? fmt::format(";var={:.3e};var_first={:.3e}",
                                                      h.var, h.var_first)
                                        : std::string{})
                                 // Cold anchor fallback (§15): the per-handover
                                 // candidacy record, that is, how many better-ranked
                                 // candidates this successor was chosen over, and
                                 // the first one refused.  Absent with the knob off,
                                 // so rows without the card are byte-identical.
                                 + (ctx.knobs.pd_candidacy_usable
                                        ? fmt::format(";cand_skipped={};cand_first_unusable={}",
                                                      h.cand_skipped,
                                                      h.cand_first_unusable.empty()
                                                          ? std::string("-")
                                                          : h.cand_first_unusable)
                                        : std::string{}));
            // Local-carry hygiene (1(d)): the marked families' phase-leveling carries
            // are erased at every station before PASS B consumes the carry (the eq34
            // D4 placement), in the `if (!pa_reref.empty())` block below.  Counted for
            // the run-end summary; the counter takes only this election's new marks (a
            // set persisting across a withheld epoch must not be counted twice).  An
            // empty set (knob off, or a quiet election) is a no-op, and re-erasing a
            // persisting set is a no-op too.
            // ---- Every handover declared (RF-2b item 2) ----------------------------
            // This loop sees every handover the election produced.  It must not sit
            // inside `if (!pa_reref.empty())`, whose predicate (whether the
            // anchor-reref marking fired this epoch) is unrelated and lets only a
            // fraction of the handovers through.
            //
            // A handover is declared to the consumer when its basis value changed,
            // that is, when the emitted phase bias steps.  A continuous handover
            // inherits the outgoing frame by construction (Δ = 0), and marking it
            // would force a consumer reset for a wire that did not move (RF-2 marked
            // them, and its PB marks rose to 5.3× the baseline against a 1.5× bar).  A
            // cold handover pins the successor at 0 against an outgoing basis of
            // `prev_value`, so it steps by exactly that, and the consumer must be told.
            // Every handover is traced; only a real step is marked, and the row
            // carries `marked=` so the two populations are separable offline.
            //
            // The mark goes through `ura_retro_sats`, the retro-bump queue that
            // NetworkSolPrep drains into markSatDiscontinuity with mark-equals-land
            // semantics: a satellite withheld this epoch stays pending and is marked
            // at its next actual emission, which is the epoch the consumer needs the
            // reset.
            if (ctx.knobs.pd_held_basis)
                for (const auto& h : handovers) {
                    const auto pk = StateKeys::parseAmbiguityKey(h.amb);
                    if (!pk) continue;
                    const auto be = ctx.carry.phase_basis.find(h.amb);
                    const double dlt =
                        (be != ctx.carry.phase_basis.end())
                            ? (be->second.value - be->second.prev_value) : 0.0;
                    // Cold-mark rule (RF-2d, network.phase_datum_held_mark_cold):
                    // |value − prev_value| is the between-station level difference, not
                    // the wire step.  A continuous pin is the successor's level in the
                    // outgoing frame, so the family does not step while the difference
                    // is non-zero; a cold pin at 0 steps the family by the successor's
                    // level, and out of a cold tenure the difference reads 0 − 0.  The
                    // Δ predicate is therefore inverted (measured on both beds in
                    // RF-3-read), and the armed rule is the frame's own: a handover is
                    // declared if and only if it is cold.
                    const bool steps = ctx.knobs.pd_held_mark_cold
                                           ? !h.continuous
                                           : std::abs(dlt) > kStsExactTolM;
                    if (steps) {
                        ctx.carry.ura_retro_sats.insert(pk->satellite);
                        ++ctx.counters.n_pb_basis_declared;
                        if (!h.continuous) ++ctx.counters.n_pb_basis_declared_cold;
                    }
                    traces.emitEvent(se.time, "STRANSFORM",
                                     h.continuous ? "sts_applied" : "sts_cold_declared",
                                     h.amb, h.to, dlt, std::nullopt,
                                     h.continuous ? "s_transform" : "re_realize",
                                     fmt::format("from={};basis={:.6f};prev={:.6f};marked={}",
                                                 h.from.empty() ? std::string("(none)")
                                                                : h.from,
                                                 be != ctx.carry.phase_basis.end()
                                                     ? be->second.value : 0.0,
                                                 be != ctx.carry.phase_basis.end()
                                                     ? be->second.prev_value : 0.0,
                                                 steps ? 1 : 0));
                }
            if (!pa_reref.empty()) {
                ctx.carry.pa_reref_total +=
                        static_cast<long long>(pa_reref.size() - pa_before);
                // Declared anchor S-transform (RF-1 step 5, §3.6).  A frame step is a
                // known, exact basis change, so the carry is expressed in the new
                // basis instead of being discarded.  Families the transform takes
                // ownership of are removed from the erase set below (transformed or
                // erased, never both), and a cold handover (no Δ) keeps its erase,
                // which is the limit of this step and the subject of RF-2.  The
                // handover declaration is in the loop above this block (RF-2b).
                std::set<std::pair<std::string, std::string>> pa_sts_handled;
                networkAmbFloatDeclaredHandover(ctx, log, traces, stations, handovers,
                                                se.time.week, se.time.tow,
                                                pa_sts_handled);
                for (const auto& fam : pa_sts_handled) pa_reref.erase(fam);
                // With every handover declared and the basis held (RF-2 item 2), the
                // network-wide erase is retired: the carries are not stale, they are
                // expressed against a datum whose move the consumer has been told
                // about.  The erase counter is therefore expected to read 0 on an armed
                // arm; the erase stays in the code so the off path and every other
                // datum realization keep it unchanged.
                //
                // Cold release (RF-2d, network.phase_datum_held_cold_release): that
                // premise holds for continuous handovers only, and R2 already keeps
                // those out of this set.  What reaches here are the cold and
                // first-sighting families, whose frame did step.  Clearing them leaves
                // their retired-frame carries folding against the re-realized design,
                // and the satellite clock absorbs the step (65 m on sim G29,
                // RF-3-read).  Armed, the set is kept and released as on the level
                // vehicle.
                if (ctx.knobs.pd_held_basis && !ctx.knobs.pd_held_cold_release)
                    pa_reref.clear();
            }
            if (!pa_reref.empty()) {
                // With the trace armed, the entries are captured before they are
                // destroyed: this scope owns the station names and the epoch stamp,
                // which the map operation does not need to know (RF-1 §7.4 D3).
                std::vector<std::pair<std::tuple<int, std::string, std::string>,
                                      LocalCarry>> pa_reref_erased;
                // The capture is also taken when a drop edge was handled, so the
                // gate-drop arc counter does not read 0 on a run that leaves
                // network.trace_amb_state off; the row writer itself stays
                // knob-gated and does nothing there (card EV7 §6).
                const int n_erased = networkApplyPhaseAnchorCarryReref(
                        pa_reref, ctx.carry.local_carry,
                        (ctx.knobs.trace_amb_state || !gate_drop_fams.empty())
                            ? &pa_reref_erased : nullptr);
                for (const auto& [k, lc] : pa_reref_erased) {
                    const int sidx = std::get<0>(k);
                    // The two populations that share this erase path are tagged
                    // apart in network_amb_state.csv: a handover or first-sighting
                    // re-reference (`anchor_reref`) and a coverage-gate drop edge
                    // (`gate_drop`).  They are disjoint by construction, so the test
                    // is a lookup, not a precedence rule (card EV7 §6, F2).
                    const bool is_gd =
                        gate_drop_fams.count({std::get<1>(k), std::get<2>(k)}) > 0;
                    if (is_gd) ++ctx.counters.n_pd_gate_drop_arcs;
                    ambStateNoteErase(
                        ctx, se.time.week, se.time.tow,
                        (sidx >= 0 && sidx < static_cast<int>(stations.size()))
                            ? stations[sidx].name : std::string{},
                        std::get<1>(k), std::get<2>(k), lc,
                        is_gd ? "gate_drop" : "anchor_reref");
                }
                ctx.carry.pa_reref_erase_total += n_erased;
                ctx.counters.n_cr_phase_datum_reref += n_erased;   // retirement census: frame step
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbAnchorRerefErase,
                           n_erased);   // card PRM phase 3a
                log->debug("Network[1(d)]: {} marked (sat,signal) family(ies) "
                           "marginalized ({} new this election); {} phase-"
                           "leveling local carry(ies) erased — refloat next "
                           "epoch.",
                           pa_reref.size(), pa_reref.size() - pa_before,
                           n_erased);
            }
        }  // end anchor election (!eq34)
    }

    // RPB allocation (card 3 eq34, rule D3c): a present station gets the
    // per-(receiver,system,signal) split state δ_{r,σ} if and only if it carries an
    // ungated shared phase-bias column on σ (a phase-bias state whose key is in
    // shared_phase_keys), that is, if it is a vertex of the D2 graph.  A station whose
    // σ-keys are all gated allocates no RPB and keeps the legacy lumping.  The set is
    // re-derived each epoch (allocate what is needed, prune the rest), before
    // est.reset() and PASS B, so buildJacobianRow's +1 RPB coefficient sees the column
    // and splitStation routes it into the local block A_r.  A spanning tree touches
    // every vertex, so every RPB-carrying station carries ≥1 tree-edge pin ⇒ its N_rr
    // stays PD.  Inert (no allocation) when eq34 is off.
    if (phase_datum_eq34) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            StateManager& sm = *stations[i].sm;
            std::set<std::string> need_rpb;
            for (const auto& kv : sm.getSatPhaseBiasStates()) {
                if (!shared_phase_keys.count(kv.first)) continue;   // gated → legacy lump
                const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                if (!p || p->satellite.empty()) continue;
                need_rpb.insert(StateKeys::makeReceiverPhaseBiasKey(
                    systemName(systemOfSat(p->satellite)), p->signal));
            }
            for (const auto& rk : need_rpb)
                if (sm.getReceiverPhaseBiasIndex(rk) < 0)
                    sm.addReceiverPhaseBiasState(rk, 0.0);
            sm.pruneReceiverPhaseBiasStatesExcept(need_rpb);
        }
    }

    // Shared-column registration, counts and estimability descriptors are in
    // ApplicationNetworkAssembly.cpp (networkAssemblyRegisterColumns_ below).  The
    // outputs are fresh per epoch; the empty-system skip keeps its bookkeeping inside
    // (SkipEpoch return), and the Next return below bypasses subEnd(SB_PREASM).
    // est.beginEpoch() runs inside on Proceed.
    // --- Level-authority census (LV-6) --
    // Serial, after PASS A's CS-GF advance; it runs before the RegisterColumns
    // SkipEpoch below (an empty-system epoch still gets its census row) and upstream
    // of every withhold decision (the census measures through withhold eras).
    // Measure-only; no-op unless armed.
    networkLevelAuthorityCensus(ctx, config_, log, traces, se,
                                setup.stations, modeled, present);

    std::vector<std::string>& registered_clock_sats = ep.columns.registered_clock_sats;
    std::vector<std::pair<std::string, std::string>>& registered_code_keys = ep.columns.registered_code_keys;
    std::vector<std::string>& col_desc = ep.columns.col_desc;
    int& n_clock = ep.columns.n_clock;
    int& n_pb = ep.columns.n_pb;
    int& n_cb = ep.columns.n_cb;
    int& n_present = ep.columns.n_present;
    if (networkAssemblyRegisterColumns_(se, ctx, setup, traces, est, present,
                                        shared_phase_keys, registered_clock_sats,
                                        registered_code_keys, col_desc,
                                        n_clock, n_pb, n_cb, n_present)
        == EpochAction::SkipEpoch)
        return LoopSignal::Next;

    // --- SD EWL Route-B integer decision (increment 2a, filter/NetworkSdEwlAr) ----
    // Its position is fixed by four constraints: after networkAssemblyPrefilter_
    // (satellites seen by fewer than 2 stations are erased there, so an integer
    // decided for one that then vanishes is wasted), after the phase-datum election
    // and the eq34 RPB allocation (both mutate the StateManager layout), after
    // networkAssemblyRegisterColumns_ (so the epoch is committed; that site can still
    // SkipEpoch), and before networkPassB_, whose parallel produce loop reads the
    // published map read-only.  It runs inside the existing SB_PREASM bracket rather
    // than a new SubBlock, because a new timer slot would change the section-timing
    // artifact for every run, armed or not.  Off by default ⇒ clears the map and
    // returns ⇒ byte-identical.
    networkSdEwlAr(ctx, config_, log, traces, setup, se, pl_now_s, modeled, present);

    // --- Oracle ambiguity pin, simulation only (filter/NetworkOraclePin) --------
    // Its position is fixed by three constraints: after networkElectPhaseAnchors
    // (the fit reads this epoch's anchors and their enforced pin targets; a handover
    // moves a family's frame term and the pin must follow it), after
    // networkAssemblyRegisterColumns_ (so the epoch is committed; that site can still
    // SkipEpoch), and before networkPassB_, whose parallel produce loop reads the
    // published table read-only.  It runs inside the existing SB_PREASM bracket rather
    // than a new SubBlock, because a new timer slot would change the section-timing
    // artifact for every run, armed or not (as for networkSdEwlAr above).  With no
    // `oracle_pin` event the pointer is null, no branch is reachable, and the output
    // is byte-identical.
    if (ctx.carry.oracle_pin)
        networkOraclePinEpoch(ctx, config_, stations, se.time, log, traces);

    // The bounded drop (RF-1 step 4, docs/refound/RF-1_float_ambiguities_design.md
    // §3.4), applied serially here: after PASS A's slip scans have latched
    // relevel.free_ambs and before the parallel PASS-B inject reads the carry.  By the
    // time a station's produce sees an arc, the drop has happened, and the inject folds
    // the arc at birth strength instead of skipping it.  A no-op unless the vehicle is
    // float_kf.  `modeled` carries this epoch's LOS elevations, with which the birth
    // floor is mapped; the map is built only when the floor is armed.
    {
        std::vector<std::map<std::string, double>> amb_elev_of;
        if (ctx.knobs.amb_birth_floor_on) {
            amb_elev_of.resize(modeled.size());
            for (std::size_t mi = 0; mi < modeled.size(); ++mi)
                for (const auto& mo : modeled[mi])
                    amb_elev_of[mi][mo.satellite] = mo.elevation;
        }
        // Card PRM phase 3a: the latch per cause, before the response reads it.
        networkResponseLatchTally(ctx);
        networkAmbFloatApplyDrops(ctx, config_, log, stations, amb_elev_of,
                                  se.time.week, se.time.tow, pl_now_s);
    }
    // --- Exact coupling time update, per-epoch inputs (card CTU) ---------------
    // After the columns are registered (the layout the rows resolve against) and
    // before PASS B, whose parallel produce reads the result.  The rows read this
    // epoch's D2 fold decision, which card PRM phase 1b makes once here (after the
    // seam's level noise is set) and the fold, later in networkSolveGlue, reads too.
    // Inside the SB_PREASM bracket for the reason networkSdEwlAr gives.  No-op off.
    networkClockLevelRestoreRaise(ctx, log);   // card CTU G5: once, the first leg epoch after a warm resume
    // Card PRM phase 2 (network.handover_stransform_before_rows): a clock-pivot handover's
    // S-transform before the decision and PASS B, through the traversal of carried
    // references, so the fold decision, the exact update's prepare and the coupling rows
    // read the re-expressed carry; the solve glue's transform then does the bookkeeping.
    // After the restore raise, so the two compose in the order the solve glue applied
    // them.  No-op off (it clears its per-epoch record and returns).
    networkHandoverStransformBeforeRows(ctx, config_, log, traces, setup.sat_health, est,
                                        se.time, registered_clock_sats);
    networkCarryDecide(ctx, est, se.time, dt);
    networkCouplingExactPrepare(ctx, est, se.time, dt, log);
    networkSubEnd(run, SB_PREASM);   // close pre-assembly; the assembly wall below is timed separately (D0b)

    // --- PASS B (in ApplicationNetworkAssembly.cpp): networkPassB_ owns the D0
    // assembly wall (a raw chrono pair; the sinks are passed by reference), the T5
    // sh-dev aging, the card-1d reinit history, the parallel produce loop (including
    // the coupling-injection cluster, whose row builder is filter/NetworkCoupling
    // since INC-7b), and the serial consume.  The failed / empty-epoch skip keeps its
    // bookkeeping inside (SkipEpoch return).
    ep.assembly.asm_res = std::vector<StationAssembly>(stations.size());
    std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    int& total_pins = ep.assembly.total_pins;
    // est-station index per station (the order addStation was called) so the
    // post-solve back-substitution can recover each present station's local
    // slant-iono block; -1 for stations not added this epoch.
    ep.assembly.est_index_of = std::vector<int>(stations.size(), -1);
    std::vector<int>& est_index_of = ep.assembly.est_index_of;
    if (networkPassB_(se, ctx, setup, traces, est,
                      carry_slipped_ambs, carry_slipped_raw,
                      carry_warm, pl_now_s,
                      modeled, present, shared_phase_keys,
                      registered_clock_sats, registered_code_keys,
                      n_clock, n_pb, n_present,
                      asm_res, est_index_of, total_pins,
                      sec_asm_ms_epoch, timing.asm_wall_ms) == EpochAction::SkipEpoch)
        return LoopSignal::Next;
    networkSubBegin(run, SB_GAUGEBUILD); // carry-driver lambda, pre-DD-build (D0b)

    // Reverse map (group key → chord ArcKey → ambiguity state key) so a
    // post-solve innovation flag (group, chord) maps back to its (station, ambiguity) for
    // the next epoch's slip funnel.  Rebuilt with the blocks each buildCarryDdPriors call
    // (the last call before the solve is authoritative).  Empty unless carry_innov_gate.
    std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key = ep.dd_build.carry_arc_amb_key;

    // --- Arc-resumption bridge classification.  Instrumentation only: nothing
    //     below changes a decision.  Of the arcs that came back after a tracking
    //     break whose raw level shifted, it counts how many re-entered carrying
    //     state that bridged the break (the harmful case) and how many re-entered
    //     as a fresh float (benign).
    //
    //     The position matters: after the slip drains, so carry_slipped_ambs (the
    //     only thing that forces a chord to re-tap) is complete for this epoch; and
    //     before buildCarryDdPriors re-keys the carry, so carriedChordValue,
    //     carry_sat_vertex and carry_group_id still describe the state the arc is
    //     returning to.  carry_group_id is read with find(), never operator[]:
    //     minting a group id here would corrupt the driver's stable keying.
    //
    //     The unit is the (station, satellite, signal) arc, not the resumption.
    //     Chords are per arc, so a dual-band satellite returning once contributes
    //     two chords that can bridge independently.  The classification is armed
    //     by the instrument or by the response
    //     (network.cycle_slip.gap_reentry_relevel): with only the response armed,
    //     the ledger is the before/after measurement of what the response did, and
    //     gating it on the diagnostic would let a response-only arm report its own
    //     effect as a structural zero.  It reads the arm that collects its
    //     population (SlipQcParams::collectsResumptions).
    if (slip_qc.params.collectsResumptions() && !slip_qc.epoch.resumptions.empty()) {
        for (const auto& g : slip_qc.epoch.resumptions) {
            const int i = g.station;
            if (i < 0 || i >= static_cast<int>(stations.size())) continue;
            const char cons =
                g.satellite.empty() ? '\0' : letterOfSat(g.satellite);
            const auto svit = carry_sat_vertex.find(g.satellite);
            for (const std::string& sig : {g.sig1, g.sig2}) {
                if (sig.empty()) continue;
                const std::string amb_key = StateKeys::makeAmbiguityKey(g.satellite, sig);
                // "Marked slipped" covers both vehicles.  carry_slipped_ambs is
                // the card-4 C3c funnel, the only thing that forces a DD chord to
                // re-tap; on the leveling vehicle, what stops the stale value
                // from folding is the free-epoch latch (`relevel.free_ambs`, read
                // by the PASS-B inject site).  A classification that read only
                // the funnel would report a freed leveling arc as
                // `shifted_bridged`, blind to its own response, and the §G3
                // response could not be graded on the lineage it was built for.
                const bool slip_marked =
                    carry_slipped_ambs.count({i, amb_key}) != 0
                    || ctx.carry.relevel.free_ambs.count({i, amb_key}) != 0;
                // (a) Legacy phase-leveling carry: a value inside the
                //     max-gap window is exactly what the PASS-B injector
                //     would fold across the break.
                bool pl_bridged = false;
                {
                    const auto cit = local_carry.find(std::make_tuple(i, g.satellite, sig));
                    if (cit != local_carry.end()) {
                        const double gap = pl_now_s - cit->second.last_seen_s;
                        pl_bridged = (gap >= 0.0 && gap <= pl_max_gap_s);
                    }
                }
                // (b) Card-4 invariant DD carry: whether the driver still holds a
                //     carried chord for this arc from before the break.
                //     carry_sat_vertex already stores the kSatBase-offset vertex
                //     id, so it is used as is; adding the base again would look
                //     up a chord that cannot exist and report "never bridged" for
                //     everything.
                bool dd_bridged = false;
                if (carry_invariant && svit != carry_sat_vertex.end() && cons != '\0') {
                    for (const auto& [gkey, gid] : carry_group_id) {
                        if (gkey.first != cons || gid == 0) continue;
                        if (gkey.second != sig) continue;   // the per-band group of `sig`
                        double zg = 0.0, vg = 0.0;
                        if (carry_driver.carriedChordValue(gid, ArcKey(i, svit->second),
                                                           zg, vg)) {
                            dd_bridged = true;
                            break;
                        }
                    }
                }
                const bool bridged = (pl_bridged || dd_bridged) && !slip_marked;
                // The arc ledger mirrors the resumption's three-way split
                // (shifted / measured and unshifted / unmeasurable), and every
                // cell is counted, so each row closes as bridged + fresh instead
                // of leaving the missing cell to be inferred (H2).  Unmeasurable
                // is kept separate from clean: an arc that bridged a break whose
                // level could not be checked is unknown, not benign, and G3's
                // "shifted-and-bridged ≈ 0 ⇒ defer" ruling holds only as far as
                // that count is small.
                if (g.shifted) {
                    if (bridged) ++slip_qc.counters.gap_shifted_bridged;
                    else         ++slip_qc.counters.gap_shifted_fresh;
                } else if (g.measurable) {
                    if (bridged) ++slip_qc.counters.gap_clean_bridged;
                    else         ++slip_qc.counters.gap_clean_fresh;
                } else {
                    if (bridged) ++slip_qc.counters.gap_unmeas_bridged;
                    else         ++slip_qc.counters.gap_unmeas_fresh;
                }
                // The per-arc event carries the same three-way verdict as the
                // counters above: an unmeasurable resumption is tagged
                // `unmeas_*`, never `clean_*`, and its ΔGF is written blank, as
                // its ΔMW is.  A "dGF=0.0000" on an arc with no pre-gap level
                // would read as "the level did not move", a claim the instrument
                // has no evidence for.
                emitEvent("T0_gap", "arc_resume", g.satellite, stations[i].name,
                          g.measurable ? std::optional<double>(g.d_mw) : std::nullopt,
                          std::nullopt,
                          g.shifted     ? (bridged ? "shifted_bridged" : "shifted_fresh")
                          : g.measurable ? (bridged ? "clean_bridged"   : "clean_fresh")
                                         : (bridged ? "unmeas_bridged"  : "unmeas_fresh"),
                          fmt::format("{};gap_ep={};dGF={};pl={};dd={};slip={}",
                                      sig, g.gap_epochs,
                                      g.measurable ? fmt::format("{:.4f}", g.d_gf)
                                                   : std::string{},
                                      pl_bridged ? 1 : 0, dd_bridged ? 1 : 0,
                                      slip_marked ? 1 : 0));
            }
        }
    }

    // Builds this epoch's DD-prior blocks from the eq34 election tree and folds the
    // carried invariant DDs into the producer solve.  Each arc (receiver, satellite)
    // ambiguity on a (const,signal) is resolved to its estimator (station, local
    // ambiguity column); pinned_amb_targets marks the spanning-tree edges (the gauge),
    // and the rest are chords (the carried DDs).  GLONASS (FDMA) is excluded.  Vertex
    // and group ids are stable across epochs (anchored on the satellite label;
    // receiver = app index).  Re-invoked on the RAIM re-solve (est.beginEpoch there
    // clears the priors and rebuilds est_index_of).  A no-op, byte-identical, unless
    // carry_invariant.
    //
    // The slip feed is the PASS-A funnel (carry_slipped_ambs, above) and, on a
    // within-epoch RAIM re-solve, the tier's culprit sets (spec §3 culprit withhold),
    // read from the epoch workspace's ws.we_culprits.  The solve glue's T2 block
    // adapts that member while it runs and clears it when it ends (card WCF), so the
    // primary build here reads it empty.
    auto buildCarryDdPriors = [&]() {
        networkBuildCarryDdPriors(ctx, config_, log, carry_driver, est, stations, asm_res,
                                  est_index_of, shared_phase_keys, carry_arc_amb_key,
                                  carry_slipped_ambs, carry_slipped_raw, ws.we_culprits);
    };
    networkSubEnd(run, SB_GAUGEBUILD);   // close gauge build and carry-driver setup (D0b)
    networkSubBegin(run, SB_DDBUILD);    // carry_driver.beginEpoch + buildCarryDdPriors, W·Pᶻ·Wᵀ (D0b)
    if (carry_invariant) {
        // The z-store predict (q_z·Δt on the carried DD covariance) runs once per
        // epoch; the build and fold may re-run on the RAIM re-solve.  D5 v2 passes the
        // epoch time (σ̂₀² reporting bucket) and a clean flag, always clean here: under
        // carry_invariant the eq34 datum re-realizes freely, so there is no re-level
        // burst to gate, and the robust geometric mean absorbs the event tail at
        // content hour 4 on its own.
        const double dt = std::isfinite(carry_prev_tow) ? (se.time.tow - carry_prev_tow) : 0.0;
        carry_driver.beginEpoch(dt > 0.0 ? dt : 0.0, se.time.tow, /*clean=*/true);
    }
    { CarryEigenScope es(carry_par, n_threads); buildCarryDdPriors(); }  // parallel_carry: thread W·Pᶻ·Wᵀ
    networkSubEnd(run, SB_DDBUILD);      // close buildCarryDdPriors, the card-4 DD-cov projection (D0b)
    networkSubBegin(run, SB_SOLVEGLUE);  // pre-solve prep + clock predict + solve + within-epoch re-solve (D0b)
    ep.solve = networkSolveGlue(ctx, config_, log, traces, setup, est, ws, se, asm_res, est_index_of,
                                present, shared_phase_keys, registered_clock_sats, registered_code_keys,
                                col_desc, n_clock, n_pb, n_cb, n_present, total_pins,
                                carry_warm, dt, pl_now_s,
                                buildCarryDdPriors);
    NetworkSolveGlueResult& sg_ = ep.solve;
    if (sg_.action == EpochAction::SkipEpoch) return LoopSignal::Next;
    auto& x_s = sg_.x_s;
    auto& plan = sg_.plan;
    auto& changed_consts = sg_.changed_consts;
    auto& datum_changed = sg_.datum_changed;
    auto& clk_model_warm = sg_.clk_model_warm;
    auto& clock_refloat_sats = sg_.handoff.clock_refloat_sats;
    auto& retro_flag_sats = sg_.handoff.retro_flag_sats;
    auto& retro_feed = sg_.retro_feed;
    auto& n_arc_refloat_epoch = sg_.handoff.n_arc_refloat_epoch;
    auto& n_adapt = sg_.n_adapt;
    auto& sigma0_adapt = sg_.sigma0_adapt;
    auto& perconst_active = sg_.perconst_active;
    auto& perconst_publish = sg_.perconst_publish;
    auto& perconst_dec_withhold = sg_.perconst_dec_withhold;
    auto& perconst_stats = sg_.perconst_stats;
    auto& we_t2_opp = sg_.we_t2_opp;
    auto& we_t2_flag = sg_.we_t2_flag;
    networkSubEnd(run, SB_SOLVEGLUE);   // close solve glue; net excludes the internal solve/reduce (D0b)
    networkSubBegin(run, SB_INSTALL);   // carry_driver.installPosterior, DD posterior mean/cov readback (D0b)
    // Installs the DD posterior (mean and covariance) back into the carry from the
    // final accepted x_s and datum: the invariant DDs the next epoch projects forward.
    // It runs after acceptance (after the skip paths of the observability and σ̂0
    // withhold gates above), so a withheld or contaminated epoch never enters the
    // carry, and outside the within-epoch RAIM block, so it runs on every accepted
    // epoch, not only RAIM-armed ones.  A failure resets the carry (a safe cold
    // start).  No-op when off.
    if (carry_invariant) {
        networkInstallCarry(ctx, config_, log, traces, carry_driver, est, x_s, plan, se, stations,
                            slip_qc, carry_arc_amb_key, carry_par,
                            n_threads, seam_threads);
    }
    networkSubEnd(run, SB_INSTALL);     // close installPosterior, the card-4 posterior readback (D0b)
    networkSubBegin(run, SB_NETAR);     // producer-side network AR phase 2, log-only, off in the tier (D0b)
    ep.ar_fixes = networkProducerAr(ctx, config_, log, traces, se, est, x_s, stations,
                                    est_index_of, asm_res, plan, log_dir_);
    const ArEpochFixes& ar_fixes = ep.ar_fixes;
    networkSubEnd(run, SB_NETAR);      // close producer AR / pre-emit tail (D0b)
    networkSubBegin(run, SB_SOLPREP);  // toSolutionEpoch + per-satellite discontinuity signalling (D0b)
    // Decision half of the gross-LOS screen (RF-3b B3,
    // docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  It runs ahead of the emission,
    // so a declaration reaches the epoch it declares rather than the one after it.  The
    // response half stays at the post-harvest site below, where it must be.  It runs
    // inside this existing bracket rather than behind a new timer slot, because a new
    // slot would change the section-timing artifact for every run, armed or not (as
    // for networkSdEwlAr).  Unreachable unless network.gross_los_emit_exclude.
    if (ctx.knobs.gl_emit_exclude)
        networkGrossLosScreen(ctx, config_, log, traces, se, stations, est, x_s,
                              asm_res, est_index_of, pl_now_s,
                              GrossLosPhase::Decide);
    // The fixed-cycle fold (card FCF, filter/NetworkFixedCycles.hpp) and the ledger of
    // fixed cycles it steps (filter/NetworkArLattice.hpp, card AGT's lattice, moved here
    // from SB_NETAR at step FCF-3): the search's fixed L1/L2 pairs, or the pairs the ledger
    // implies, folded into a held solve beside the float one.  After the gross-LOS Decide,
    // so its declarations release this epoch, and inside this bracket for the reason given
    // above.  Under fold emit with an accepted held solve, solution prep and the emitter
    // read the held vector (x_emit); every other seam keeps the float x_s.  Unreachable
    // unless the fold is armed or network.ambiguity_resolution.trace_lattice.
    ep.fixed_cycles = networkFixedCycles(ctx, config_, log, traces, se, est, x_s,
                                         plan.shared_datum, ar_fixes, sg_.we_landed,
                                         stations);
    const Eigen::VectorXd& x_emit = ep.x_emit();
    ep.prep = networkSolPrep(ctx, config_, log, traces, setup, se, est, x_emit,
                             plan, pl_now_s, datum_changed, changed_consts,
                             sigma0_adapt, perconst_active, perconst_publish,
                             perconst_dec_withhold, perconst_stats);
    NetworkSolPrepResult& sp_ = ep.prep;
    auto& sol = sp_.sol;
    auto& epoch_withheld_consts = sp_.epoch_withheld_consts;
    auto& global_bump = sp_.global_bump;
    auto& serialized_consts = sp_.serialized_consts;
    auto& retro_emit_sats = sp_.retro_emit_sats;
    networkSubEnd(run, SB_SOLPREP);     // close sol-prep and discontinuity signalling (D0b)
    // Whether this epoch's solution reached the wire.  Every whole-epoch withhold
    // (observability gate, A4 sigma0 ceiling) `continue`s or returns before
    // networkEmitStore's emit site, and n_emitted is incremented exactly there,
    // so comparing that counter across the call tests emission.  The tables
    // written once after the first emitted epoch (signals as planned, parameters as
    // armed, contributions as armed) key on it below.
    const std::size_t n_emitted_before = ctx.counters.n_emitted;
    // The smoothed hold's clock clones (card SMH): advanced over this epoch before the
    // hold reads them at emit.  No-op off.
    networkPbHoldCloneUpdate(ctx, log, est, x_s, ws, plan.shared_datum);
    networkSubBegin(run, SB_EMITSTORE); // block 7: re-base + store push + solution CSV (D0b)
    networkEmitStore(ctx, config_, log, traces, setup, se, est, x_emit, pl_now_s,
                     datum_changed, sigma0_adapt, sol, epoch_withheld_consts, global_bump,
                     serialized_consts, retro_emit_sats, emit_engines, present,
                     est_index_of, modeled, shared_phase_keys, carry_slipped_ambs,
                     we_t2_opp, we_t2_flag, cl_tally);
    networkSubEnd(run, SB_EMITSTORE);   // close block 7: emit/store, order-sensitive, serial (D0b)
    ep.emitted = ctx.counters.n_emitted != n_emitted_before;
    networkSubBegin(run, SB_IONODIAG);  // block 4a: iono / ZWD / anchor-info viz traces (D0b)
    networkIonoDiag(ctx, config_, log, traces, setup, ws, se, est, x_s, plan, asm_res,
                    est_index_of, modeled, registered_clock_sats);
    networkSubEnd(run, SB_IONODIAG);   // close block 4a, iono/ZWD/anchor traces (D0b)
    networkSubBegin(run, SB_DDFRAC);   // block 6: cross-station DD fractional-residual trace, B5 (D0b)
    networkDdFrac(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                  est_index_of);
    networkSubEnd(run, SB_DDFRAC);       // close block 6, dd-frac B5 (D0b)
    networkSubBegin(run, SB_SHDEV_VTEC); // block 4b: T5 SH-deviation carry KF + VTEC coefficient trace (D0b)
    networkShDevVtec(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                     est_index_of, modeled);
    networkSubEnd(run, SB_SHDEV_VTEC);  // close block 4b, T5 sh-dev + VTEC trace (D0b)
    networkSubBegin(run, SB_T1A);       // block 3: T1a SH health probe (D0b)
    networkShHealth(ctx, config_, log, traces, setup, se, est, x_s, asm_res, est_index_of,
                    modeled, carry_vce_est, prires_est, datum_changed, pl_now_s);
    networkSubEnd(run, SB_T1A);        // close block 3, T1a health (D0b)
    networkSubBegin(run, SB_LEVELING); // block 5: phase-leveling carry harvest (D0b)
    networkLeveling(ctx, config_, log, traces, setup, se, est, x_s, plan, asm_res,
                    est_index_of, carry_vce_est, prires_est, carry_warm, pl_now_s,
                    registered_clock_sats, registered_code_keys, shared_phase_keys);
    networkSubEnd(run, SB_LEVELING);   // close block 5, leveling (D0b)
    networkSubBegin(run, SB_RAIM);     // block 2: post-fit network RAIM + median-polish attribution (D0b)
    // RAIM pool re-host: `carry_warm` requires network.phase_leveling, so gating on
    // it alone would leave post-fit RAIM unreachable on every card-4 lineage that
    // leaves leveling off (armed at load, never executed).  ctx.knobs.raim_ci_on is
    // the re-hosted arming condition (already ANDed with raim_on and the resolved
    // carry_invariant).
    if (raim_on && (carry_warm || ctx.knobs.raim_ci_on)) {
        networkPostFitRaim(ctx, config_, log, traces, se,
                           stations, asm_res, est_index_of, present, x_s, est,
                           plan.shared_datum, clock_refloat_sats, retro_flag_sats,
                           retro_feed, n_arc_refloat_epoch, prires_est);
    }
    // Gross line-of-sight screen (card GL, docs/SPEC_event_harness.md §24.18), the
    // only acting post-fit tier that screens code.  It sits inside this bracket,
    // immediately after the RAIM pass, for the ordering reason RAIM's own header
    // gives: erasing a `local_carry` entry is the refloat, so it must run after the
    // carry harvest (SB_LEVELING) and before the epoch stash tail.  It has no arming
    // interlock on RAIM: it owns its own residual walk and its own state because
    // every other tier is phase-only, and it must be able to run on a lineage that
    // arms none of them.  No-op (not even a state allocation) unless
    // network.gross_los_screen.
    // With network.gross_los_emit_exclude armed (RF-3b B3,
    // docs/refound/RF-3_code_row_qc_2026-09-22.md §4), the decision half already ran
    // above, before the emission, and this call is the deferred response half, which
    // belongs at this site for the ordering reason above.  Unarmed, this one call does
    // both halves.
    networkGrossLosScreen(ctx, config_, log, traces, se, stations, est, x_s,
                          asm_res, est_index_of, pl_now_s,
                          ctx.knobs.gl_emit_exclude ? GrossLosPhase::Act
                                                    : GrossLosPhase::Full);
    networkSubEnd(run, SB_RAIM);       // close block 2, RAIM (D0b)
    networkSubBegin(run, SB_CLOCKUPD); // block 8: two-state clock update + fault QC + retro-flag feed (D0b)
    networkClockQc(ctx, config_, log, traces, ws, se, est, x_s,
                   registered_clock_sats, datum_changed, changed_consts,
                   clk_model_warm, pl_now_s, dt,
                   clock_refloat_sats, retro_flag_sats, retro_feed);
    networkSubEnd(run, SB_CLOCKUPD);       // close block 8, clock update + retro feed (D0b)
    // The phase-visible consistency instrument (card PVN): measure-only, after the
    // epoch's last re-solve, while the fold capture in ws still holds the prior.
    networkPhaseVisibleMonitor(ctx, traces, se, est, x_s, ws);
    // Roster disagreement pass (app/SignalRoster.cpp, card SID phase 1).  It sits here
    // because this is the first point at which every input of P4 exists in one scope:
    // the arc-level tracker's rows were built in PASS A, local_carry is post-harvest
    // (networkLeveling), the ambiguity states are post-assembly, the anchor
    // incumbents are post-election, the freed-ambiguity set carries this epoch's slip
    // responses, and `sol` is the solution the wire received.  Read-only; every class
    // is a counter.  It also closes the epoch's decision-site census and, after the
    // first emitted epoch whose roster carries a modelled role, logs the "signals as
    // planned" table with its run-manifest record (card NOM step N3a).
    networkSubBegin(run, SB_ROSTER);
    // Phase 2 adds `modeled` and `present`: the admitted row set that feeds the
    // second lifecycle.  They are in the same scope as every other input of the pass,
    // which is why the pass runs here and not at the build seam.
    networkSignalRosterAudit(ctx, config_, log, traces, se, stations, modeled, present,
                             slip_qc.epoch.computed ? &slip_qc.epoch : nullptr,
                             carry_slipped_ambs, sol,
                             ep.emitted);
    // Fold half of the ledger emit (card SID phase 5a, E1).  It runs here and not at
    // the emit site because the ledger's admitted machine is advanced by the audit
    // pass immediately above: an admitted arc birth at this epoch does not exist
    // before that pass runs.  Read-only; every class is a counter plus an ARC event.
    // Disarmed, or with no capture from this epoch, it returns on its first line.
    networkLedgerEmitFold(ctx, traces, se);
    // Epoch tail of the second phase channel (card SID phase 6a, R3 + R4).  It sits
    // here for the same reason as the pass above: this is the first point at which
    // both of its inputs are final.  `relevel.free_cause` carries every tier's
    // freeing verdict for this epoch (including the post-solve RAIM ones, written at
    // SB_RAIM) and is not cleared until the next epoch's slip scan, and the
    // phase-datum anchors have been elected.  Read-only; every class is a counter.
    // Disarmed, it returns on its first line.
    networkSecondPhaseEpochTail(ctx, config_, stations);
    // All-channel census (card SID phase 6b), folded here beside the tail above
    // because it reads the same two republished cell sets plus the stations' own
    // state maps, all final at this point.  Measure-only; it returns on its first
    // line unless the widening is active.
    networkChannelAllEpoch(ctx, config_, stations, modeled, present);
    // Second-constellation census (card GAL): inter-system bias columns, satellites
    // per constellation, and the non-GPS-only station-epochs.  It sits here for the
    // same reason as the census above: `present` is final.  Measure-only; it returns
    // on its first line unless a constellation other than GPS is active.
    networkMultiGnssEpoch(ctx, config_, stations, modeled, present);
    // Parameters as armed (card NOM N1): every parameter instance with its treatment,
    // from the inventory PASS B drained, written once after the first emitted epoch.
    // Trace class; it returns on its first line once the table is out.
    networkParamInventoryEpochTail(ctx, log, traces, nc,
                                   ep.emitted);
    // Contributions as armed (card NOM N2a): every contribution to the normal equations
    // by its declared kind (the station side drained after PASS B, this epoch's shared
    // folds from the estimator), written once after the first emitted epoch.  Trace class.
    networkContributionsEpochTail(ctx, est, log, traces, nc,
                                  ep.emitted);
    networkSubEnd(run, SB_ROSTER);
    // Trigger instrumentation (SPEC_day129_frame_guards P1-3/P1-4).  It runs before
    // the stash/status block, because the epoch-status row written there reads the
    // post-fit sigma0 this fills in.  All off by default; an unarmed run makes
    // neither call (byte-identical, no cost beyond the flag tests).  The §4.3 keyed
    // trace and the per-row trace use the same residual pass, so any of the three
    // trace toggles runs it.
    networkSubBegin(run, SB_FRAMEDIAG);
    // Predict half of the block-Baarda census (BLOCKFORMAL): each family's σ̂₀
    // through the previous epoch is frozen and the per-epoch ladders are cleared
    // before the census's rider feeds this epoch's blocks.  Predict-then-update is
    // the D5 v2 contract: a block standardized by a calibration it contributed to can
    // standardize itself away.
    if (ctx.knobs.bb_on) ctx.carry.bb.beginEpoch(pl_now_s, se.time.tow);
    // Station-congruence predict half, with the same contract and reason: σ̂₀ through
    // the previous epoch is frozen before the census's rider feeds this epoch's
    // stations, so a station cannot standardize its own coordinate fault away.
    if (ctx.knobs.cg_on) ctx.carry.cg.beginEpoch(pl_now_s, se.time.tow);
    if (ctx.knobs.trace_postfit_on)
        networkPostfitSigma0(ctx, config_, log, traces, se, stations, est, x_s,
                             asm_res, est_index_of, &modeled);
    if (ctx.knobs.trace_sh_spectrum)
        networkShSpectrum(ctx, config_, log, traces, se, est);
    // Per-station local-block audit (card ZP, SPEC_event_harness §24.10).
    // Measure-only (const accessors on the solved estimator); it belongs in this
    // group because it is the station-side companion of the shared-side spectrum
    // above.  Its cadence gate (emitted epochs 1..20, then every 60th) lives in the
    // seam, so the driver carries no counter.
    if (ctx.knobs.trace_station_block)
        networkStationBlock(ctx, config_, log, traces, se, stations, est, x_s,
                            asm_res, est_index_of);
    // Prefit innovation census, seed-birth tripwire and topology ledger
    // (docs/SPEC_prefit_census_observability.md INC-1/3/4), all on one walk of this
    // epoch's assembled row stacks.  It is first in the measure-only group: it costs
    // O(rows), it writes the epoch-status scalars the stash block picks up, and it is
    // the free tier the redundancy census escalates from.  The master knob keeps an
    // unarmed run out of the translation unit entirely.
    if (ctx.knobs.pc_walk_on)
        networkPrefitCensus(ctx, config_, log, traces, se, stations, est,
                            asm_res, est_index_of);
    // Redundancy / testability census (docs/SPEC_redundancy_census.md).  It runs
    // before networkRhsAttribution: a flight-recorder trip epoch forces a full sweep,
    // and the trip predicate is the attribution's own (unconsumed) window test, so the
    // census must read it before the attribution seam advances rhs_attrib_done.
    // Measure-only; the master knob keeps an unarmed run out of the translation unit
    // entirely.
    if (ctx.knobs.redun_census_on)
        networkRedundancyCensus(ctx, config_, log, traces, se, stations, est,
                                plan.shared_datum, asm_res, est_index_of, x_s);
    // Block-Baarda drain (docs/SPEC_redundancy_census.md BB-INC-1..3): drains this
    // epoch's LOS-block ladders, writes network_block_baarda.csv and emits the block
    // identities.  The feed is inside the census above (the block sweep inherits its
    // stride and escalation instead of adding a scheduler), so this must run after
    // it.  The accumulator carries nothing forward, so an epoch that swept blocks and
    // never reached here does not report them.
    if (ctx.knobs.bb_on)
        networkBlockBaardaEmit(ctx, config_, log, traces, se, stations);
    // Congruence drain (docs/SPEC_redundancy_census.md CG-INC-1..3): drains this
    // epoch's per-station coordinate-offset records, writes network_congruence.csv and
    // emits the identities of the flagged stations.  The feed is inside the census
    // above (same host, same stride, no scheduler of its own), so this must run after
    // it.  It does not read ctx.carry.bb_hits (the separability join was resolved at
    // the sweep site), so its position relative to the block census's own emit is
    // free.
    if (ctx.knobs.cg_on)
        networkCongruenceEmit(ctx, config_, log, traces, se, stations);
    // MDB drain (docs/SPEC_redundancy_census.md MDB-INC-1): aggregates this epoch's
    // Baarda MDB rows, writes network_mdb.csv, emits the reporting events and
    // reconciles the uncheckable join.  Both orderings matter: after
    // networkPostfitSigma0, whose per-family σ̂₀ the scaled variant needs (read
    // earlier it would apply the previous epoch's inflation), and after the redundancy
    // census, whose per-epoch row counts the join compares against.  The feed sites are
    // in SB_RAIM and in the census; this is only the drain, and the accumulator is
    // stamped so an epoch that fed rows and never reached here leaks nothing forward.
    if (ctx.knobs.mdb_on)
        networkMdbFlush(ctx, config_, log, traces, se);
    // Structural S-basis dimension (docs/SPEC_datum_graph_census.md): predicted from
    // this epoch's admitted support topology and reconciled against the spectral count
    // the solve glue handed forward.  It is measure-only: it reads the registration
    // sets and `modeled`, and writes nothing but rows, events and counters.  Its order
    // within the group is free (it shares no accumulator with the censuses above), so
    // it sits last among them, ahead of the attribution seams that consume the trip
    // predicate.  The master knob keeps an unarmed run out of the translation unit
    // entirely.
    if (ctx.knobs.dg_on)
        networkDatumGraphCensus(ctx, config_, log, traces, se, stations, modeled,
                                est_index_of, plan, shared_phase_keys,
                                registered_code_keys, registered_clock_sats,
                                datum_changed);
    if (est.attribArmed())
        networkRhsAttribution(ctx, config_, log, traces, se, stations, est,
                              plan.shared_datum, asm_res, est_index_of, &x_s);
    // Flight recorder (INC-5): consumes a trip raised anywhere earlier this epoch
    // (clock runaway, a00-band exit; the A4 withhold consumes its own,
    // since that epoch never reaches here), then retains this epoch as the next trip's
    // "before" frame.  Last in the group, so a trip dump sees the same kernel state
    // every other diagnostic just read.
    networkRhsAttribTrip(ctx, config_, log, traces, se, stations, est,
                         plan.shared_datum, asm_res, est_index_of, &x_s);
    networkSubEnd(run, SB_FRAMEDIAG);
    networkSubBegin(run, SB_STASH_STATUS); // D2/C1/SH carry re-stash + VCE fold + epoch-status QC (D0b)
    networkStashStatus(ctx, config_, log, traces, setup, se, est, x_s, plan, carry_vce_est,
                       prires_est, clock_refloat_sats, n_arc_refloat_epoch, registered_clock_sats,
                       registered_code_keys, retro_emit_sats, shared_phase_keys,
                       sigma0_adapt, total_pins, dt,
                       datum_changed, changed_consts, n_clock, n_present, n_pb, n_cb,
                       n_dropped_sats, n_adapt);
    // Card SMH: the clones re-harvested on the stash just written, and the clones of
    // the generations that started at this epoch's emit born on it.  No-op off.
    networkPbHoldCloneHarvest(ctx, est, x_s, TimeUtils::continuousGpsSeconds(se.time));
    // The level monitor of card CTU reads the post-fit code and the block the stash
    // just wrote; with the bound raise armed it raises that block before the next
    // fold.  It runs only on a kept epoch, which is every epoch that reaches here.
    // No-op off.
    networkClockLevelMonitor(ctx, traces, se.time, est, x_s, asm_res, est_index_of,
                             datum_changed);
    // Drains this epoch's buffered carry-state rows (RF-1 §7.4).  It is last inside
    // the bracket because this is the first point after every row source has run: the
    // leveling harvest and the gap prune (SB_LEVELING), the T4 / T3 refloats (SB_RAIM)
    // and the phase-anchor carry re-reference (the datum block).  A no-op with an
    // empty buffer, which is every unarmed run.
    networkAmbStateTrace(ctx, log, traces);
    // Card LGM: drain the phase-colour trace rows buffered at the harvest.
    if (!ctx.carry.pc_trace_rows.empty()) {
        if (traces.phase_colour_trace.is_open())
            for (const std::string& row : ctx.carry.pc_trace_rows)
                traces.phase_colour_trace << row;
        ctx.carry.pc_trace_rows.clear();
    }
    // Card AIL: the same drain for the ambiguity information trace.
    if (!ctx.carry.amb_if_rows.empty()) {
        if (traces.amb_if_trace.is_open())
            for (const std::string& row : ctx.carry.amb_if_rows)
                traces.amb_if_trace << row;
        ctx.carry.amb_if_rows.clear();
    }
    networkSubEnd(run, SB_STASH_STATUS);   // close the stash and epoch-status tail, before the readout (D0b)

    // Per-epoch section-timer readout, emitted-epoch path (D0).  Deltas of the
    // estimator's run-cumulative accumulators against the loop-top snapshot give this
    // epoch's kernel cost.  The driver assembly wall (sec_asm_ms_epoch) covers the
    // primary produce + scatter section; reduce, solve, cov-inverse and cov-consumers
    // happen after the wall, so `other` (QC decision logic, emit, I/O, any within-epoch
    // RAIM re-solve) = total − assembly − reduce − solve − cov-inverse − cov-consumers.
    // d_estadd is the kernel est.addStation delta, a cross-cutting figure: it also
    // captures re-solve adds after the wall, so it is not a strict part of the
    // assembly wall, and it is reported separately rather than nested under it.
    if (ctx.knobs.sec_timers_on) {
        const auto& stmr = est.sectionTimers();
        const double d_total  = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sec_epoch_t0).count();
        const double d_reduce = stmr.reduce_ms        - sec_t0.reduce_ms;
        const double d_solve  = stmr.solve_ms         - sec_t0.solve_ms;
        const double d_covinv = stmr.cov_inverse_ms   - sec_t0.cov_inverse_ms;
        const double d_covcon = stmr.cov_consumers_ms - sec_t0.cov_consumers_ms;
        const double d_estadd = stmr.add_station_ms   - sec_t0.add_station_ms;
        const std::size_t d_covcalls =
            stmr.cov_consumer_calls - sec_t0.cov_consumer_calls;
        const double d_other = d_total - sec_asm_ms_epoch - d_reduce - d_solve
                               - d_covinv - d_covcon;
        ++timing.profiled_epochs;
        log->info("Network (D0) TOW {:.1f}: total {:.1f} ms | assembly-wall {:.1f} "
                  "| reduce {:.1f} | solve {:.1f} | cov-inv {:.1f} | cov-consumers {:.1f} "
                  "({} calls) | other {:.1f} | est.addStation {:.1f} (cross-cutting) | "
                  "{} shared cols, {} stations",
                  se.time.tow, d_total, sec_asm_ms_epoch, d_reduce, d_solve,
                  d_covinv, d_covcon, d_covcalls, d_other, d_estadd,
                  static_cast<long long>(est.numShared()), n_present);

        // Per-epoch sub-block attribution of `other` (D0b).  net = wall − Δ(timed
        // sections) inside each block; Σ net should approach d_other, and the residual
        // is logged as "unattributed" (loop regions this instrument does not bracket).
        std::string d0b;
        double d0b_net_sum = 0.0;
        for (int b = 0; b < SB_COUNT; ++b) {
            const double net = (timing.sub_blocks[b].wall_ms  - sec_sub_ep0[b].wall_ms)
                             - (timing.sub_blocks[b].timed_ms - sec_sub_ep0[b].timed_ms);
            d0b_net_sum += net;
            d0b += fmt::format("{}={:.1f} ", kSubBlockNames[b], net);
        }
        log->info("Network (D0b) TOW {:.1f}: other-net ms [ {}] Σnet {:.1f} | other {:.1f} "
                  "| unattributed {:.1f}",
                  se.time.tow, d0b, d0b_net_sum, d_other, d_other - d0b_net_sum);
    }

    // Periodic atomic snapshot (RTB-7(a)); the body is in NetworkStateSnapshotWrite.cpp
    // (INC-4).  It stays after the prev_week/prev_tow advance, so the snapshot
    // captures this epoch.
    networkWriteStateSnapshot(ctx, config_, se.time, pl_now_s, emitter, carry_driver,
                              stations,              // CN rows (RS-8 §21.16; the latch
                                                     //   lives in the station models)
                                                     //   and RL x_lin rows (RS-9 §21.18)
                              log,
                              &emit_engines.clk_rebase,  // frame rows, v2 (§18.10)
                              &emit_engines.rebase,      // emit-layer ER rows (RS-5 §21.10)
                              setup.consumer.get()); // consumer snapshot beside each history copy (card CC (B))
    return LoopSignal::Next;
}

} // namespace GnssProcessor
