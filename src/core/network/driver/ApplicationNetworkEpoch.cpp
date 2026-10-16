/**
 * @file ApplicationNetworkEpoch.cpp
 * @brief Application::networkProcessEpoch_: one synced epoch through the PPP-RTK network
 *        producer (card NOM3 step EP1).
 *
 * The body is runNetwork's former per-epoch lambda (processSyncedEpoch), moved four columns
 * left by scripts/nom_ep1_cut.py and checked line for line by scripts/nom_ep1_verify.py
 * (check V1a).  What the move changes, each a declared transform of the EP1 specification
 * (section 5.3): the epoch's locals live in a NetworkEpoch (network/driver/Epoch.hpp) built right
 * after the admission; the run's objects are reached through a NetworkRun
 * (network/driver/Run.hpp); the D0b brackets are networkSubBegin / networkSubEnd; a converted
 * seam takes (run, ep[, kept...][, outputs...]) and binds its former parameters in its own
 * preamble.
 *
 * Maintenance.  This file was written by the EP1 generator and cut by the EP2 generator
 * (scripts/nom_ep2_cut.py, checked by scripts/nom_ep2_verify.py): step EP2 moved its inline
 * blocks to their concerns (the admission, the slip funnel's drains, the phase datum, the
 * gap-resume census, the float drop, the DD-carry predict, the section-timer frame) and the
 * driver-side gates to their seams' first lines.  Step EP3 replaced the calls of the 21
 * measure-only instruments by six networkObserveEpoch slot calls; the rows, their slots and
 * their order constraints are network/driver/EpochObservers.hpp.  It is maintained by hand; nothing
 * regenerates it.  Two checks guard every change: `python scripts/nom_epoch_lint.py` (the
 * epoch function's reading, the post-solve write discipline, each group read only after its
 * first writer, the writer banner of network/driver/Epoch.hpp), run before any commit that touches
 * this file, a seam signature or CMakeLists.txt; and tests/network/test_network_epoch_contract.cpp,
 * whose casts pin every spine seam's epoch form at compile time.  Do not add code here;
 * CLAUDE.md's decision tree names the seam each concern belongs in.
 */

#include "app/Application.hpp"
#include "network/driver/Epoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/Run.hpp"     // NetworkRun (card NOM3 step EP1)
#include "network/driver/EpochAdmission.hpp"     // networkFastForward, networkAdmitEpoch (card NOM3 step EP2)
#include "network/slip/SlipFunnel.hpp"       // the slip funnel's drains (card NOM3 step EP2)
#include "network/solve/PhaseAnchorEpoch.hpp" // the phase datum of the epoch (card NOM3 step EP2)
#include "network/slip/GapResumeCensus.hpp"    // the arc-resumption bridge census (card NOM3 step EP2)
#include "network/driver/EpochObservers.hpp"  // the measure-only instruments, a declared table (card NOM3 step EP3)

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "network/emit/EmitRebase.hpp"   // emit-layer frame re-base
#include "network/emit/ClockEmitRebase.hpp" // clock emission re-base + per-sat step marks
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot + warm resume
#include "network/solve/Estimator.hpp"
#include "network/solve/ClockLevel.hpp"         // satellite-clock level monitor and restore raise (card CTU)
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "network/solve/Sbasis.hpp"
#include "network/solve/PivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "network/carry/CarryVce.hpp"        // LS-VCE variance-component monitor for carry rows (M2)
#include "network/qc/PriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
#include "network/carry/InvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (card 4, M1 C3b)
#include "filter/StateKeySchema.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat, rinexLetter, systemName
#include "network/driver/RunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "network/driver/WlRunTypes.hpp" // RAII and section-timer types hoisted out of runNetwork() (INC-9b)
#include "network/driver/RunGroups.hpp"  // the driver-owned run groups: engines, emit engines, timing, closed-loop tally (card NOM TD1a)
#include "network/qc/RobustStats.hpp"   // median / MAD helpers for robust attribution (INC-2)
#include "network/diag/AmbStateTrace.hpp" // per-arc carry-state trace: drain and erase capture (RF-1 §7.4)
#include "network/ar/AmbFloat.hpp"      // floored birth prior and bounded drop (RF-1 step 4)
#include "network/solve/HandoverStransform.hpp" // kStsExactTolM (RF-2b: the declaration step test)
#include "network/solve/ChainDatum.hpp"    // eq34 single-receiver-chain datum (card 3, INC-8b)
#include "network/assembly/SignalIntegrity.hpp" // flicker/gap: band-state classifier + unvetted phase demotion
#include "network/assembly/MeasurementAdmission.hpp" // product-referenced admission: the corrupt-channel screen
#include "network/qc/GrossLos.hpp"    // gross line-of-sight screen (card GL, §24.18)
#include "network/qc/Raim.hpp"        // post-fit network RAIM (INC-9)
#include "network/qc/ClockQc.hpp"     // post-solve satellite-clock QC (INC-10)
#include "network/diag/FrameDiag.hpp"   // frame-guard trigger instrumentation (P1-3/P1-4, SPEC_day129_frame_guards)
#include "network/diag/StationBlock.hpp"  // per-station local-block audit, measure-only (card ZP, §24.10)
#include "network/qc/RhsAttrib.hpp"   // reduced-RHS class attribution (era-2 injector probe)
#include "network/qc/Mdb.hpp"               // Baarda reliability numbers / MDB (SPEC_redundancy_census)
#include "network/qc/RedundancyCensus.hpp"  // redundancy / testability census (SPEC_redundancy_census)
#include "network/qc/PrefitCensus.hpp"      // prefit census / fuse / birth check / topology (SPEC_prefit_census_observability)
#include "network/solve/DatumGraph.hpp"        // structural S-basis dimension census (SPEC_datum_graph_census)
#include "network/emit/SolPrep.hpp"    // emit-side solution prep and discontinuity signalling (INC-11pre)
#include "network/emit/EmitStore.hpp"    // emit, store, product write and closed loop (INC-11)
#include "network/runend/Teardown.hpp"
#include "network/diag/PhaseVisibleDiag.hpp"  // phase-visible consistency instrument (card PVN)
#include "network/emit/PbHoldClone.hpp"       // the smoothed hold's clock clones (card SMH)
#include "network/state/CarryDisposition.hpp"  // the decided-once fold disposition (card PRM phase 1b)
#include "network/ar/ProducerAr.hpp" // log-only producer AR and NEQ export (INC-9b)
#include "network/ar/FixedCycles.hpp" // the fixed-cycle fold and its ledger of fixed cycles: a detached held solve only the emission reads (card FCF)
#include "network/carry/Inject.hpp"
#include "network/assembly/CanonicalPairAdmission.hpp" // canonical-pair admission seam (card CPA)
#include "network/assembly/SignalRoster.hpp"  // signal roster and arc-ledger observatory (card SID phase 1)
#include "network/assembly/SecondPhaseScope.hpp"  // second phase channel, epoch tail (card SID phase 6a)
#include "network/assembly/ChannelAll.hpp"      // all-channel census (card SID phase 6b)
#include "network/emit/LedgerEmit.hpp"  // emitter read against the ledger, declared roster (card SID phase 5a)
#include "network/carry/InstallCarry.hpp" // installPosterior and the WL zone-4 fold / inject (INC-9b)
#include "network/carry/DdPriorBuild.hpp" // buildCarryDdPriors free function (INC-9b)
#include "network/solve/SolveGlue.hpp" // datum, solve and within-epoch RAIM (INC-9b)
#include "network/diag/IonoDiag.hpp"    // iono / ZWD / anchor-info viz traces (INC-11b)
#include "network/diag/DdFrac.hpp"    // cross-station DD fractional-residual trace (INC-11b)
#include "network/ar/SdEwlAr.hpp"   // SD EWL Route-B integer decision (increment 2a)
#include "network/diag/ShDevVtec.hpp"    // SH-deviation carry KF and VTEC-coefficient trace (INC-11b)
#include "network/diag/ShHealth.hpp"    // SH-field health probe, carry LS-VCE (INC-11b)
#include "network/carry/LevelAuthority.hpp"  // per-epoch level-authority census (LV-6)
#include "network/diag/Leveling.hpp"    // phase-leveling carry harvest, D2/C1 stash (INC-11b)
#include "network/diag/StashStatus.hpp"    // carry re-stash, LS-VCE fold, epoch-status QC row (INC-11b)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "network/diag/RunTraces.hpp" // trace-stream ownership, open logic, roster, FlagRate (INC-3)
#include "network/state/RunContext.hpp"    // knobs, tier-1 carry state and service counters, as ctx (INC-4)
#include "network/driver/SetupResult.hpp"   // setup-phase handoff (setup) and navAvailabilityGpsSeconds (INC-5)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "network/slip/SlipQc.hpp"       // pure slip-QC helpers: ifElevMap, parseIfPairResetKey (INC-2)
#include "network/slip/SlipOracle.hpp"   // simulation-only oracle slip response (level-preserving ceiling)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // the slip-QC wavelength provider (GLONASS slots)
#include "realtime/RealTimeEngine.hpp"
#include "network/driver/StreamFrontEnd.hpp"  // multi-station streaming epoch source (RTF-3)
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
// The per-epoch body, a member function over the run (network/driver/Run.hpp), which holds
// the loop-invariant setup and the ctx-carried run state, and the synced epoch.  Until
// card NOM3 step EP1 it was runNetwork's lambda processSyncedEpoch, a callable over `se`
// that captured every other name by reference.  Loop control maps one to one (a
// top-level `continue` became return Next, the max-duration `break` return Stop), so
// runNetwork's batch iteration is structurally identical to an inline loop.  The
// function exists so a streaming epoch source (network.streaming, the RTF-3
// multi-station front-end) can drive the identical per-epoch body without a pre-built
// synced vector; both epoch sources are in runNetwork (ApplicationNetwork.cpp).
Application::LoopSignal Application::networkProcessEpoch_(NetworkRun& run,
                                                          const SyncedEpoch& se_raw) {
    // ---- The run under the names the remaining lines read (card NOM3 step EP1; generated by
    //      scripts/nom_ep1_cut.py; each line the frozen runNetwork declaration with the run's
    //      objects re-pointed).  Step EP2 removed the names only the moved blocks read. ----
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const NetworkConfig& nc = run.nc;
    NetworkSetupResult& setup = run.setup;
    auto& stations = setup.stations;
    const int n_threads = setup.n_threads;
    const bool carry_par = setup.carry_par;
    NetworkRunContext& ctx = run.ctx;
    NetworkRunTraces& traces = run.traces;
    SlipQcChannel& slip_qc = run.engines.slip_qc;
    const std::map<std::string, std::string> last_pivot =
        net::letterMap(ctx.entities.constellations, &net::ConstellationState::last_pivot);
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   ura_on        = ctx.knobs.ura_on;
    const bool   raim_on       = ctx.knobs.raim_on;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    NetworkRunTiming& timing = run.timing;
    // ---- End of the run's names ----

    // The start-offset fast-forward (network/driver/EpochAdmission.cpp): before the injector, so
    // a fast-forwarded epoch cannot consume an event.
    if (networkFastForward(run, se_raw) == EpochAdmission::Skip) return LoopSignal::Next;
    // The log prefix is bound before the injector runs, so the injector's own
    // warnings carry this epoch's bracket, not the previous one.  se.time ==
    // se_raw.time always (the injector never touches the time tag), so the
    // position of this call affects log text only.
    LogManager::setCurrentEpochTime(se_raw.time);
    // Event-harness seam (network/carry/Inject.cpp), the single observation
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
    // Canonical-pair admission seam (network/assembly/CanonicalPairAdmission.cpp,
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
    // The epoch: everything the stages below hand each other (network/driver/Epoch.hpp).
    NetworkEpoch ep(se);

    // Signal-roster seam (network/assembly/SignalRoster.cpp, card SID phase 1).  It sits
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
    networkBuildSignalRoster(run, ep);
    networkSubEnd(run, SB_ROSTER);

    // The admission (network/driver/EpochAdmission.cpp): the one-epoch clears and the broadcast
    // feeds, which run on every synced epoch, then the max-duration stop, the decimation skip
    // and the processed-epoch count.  After the roster, which advances on every synced epoch.
    const EpochAdmission admission = networkAdmitEpoch(run, ep);
    if (admission == EpochAdmission::Stop) return LoopSignal::Stop;
    if (admission == EpochAdmission::Skip) return LoopSignal::Next;
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::Admitted, ep.events);

    // Per-epoch section-timer baseline (D0, network/driver/Run.cpp); inert when the toggle is off.
    const EpochSectionTimers sec = networkSectionTimersBegin(run);

    // Rotation of the unbounded service CSVs (P1); the body is in NetworkRunTraces
    // (INC-4).  Off ⇒ a byte-identical no-op.
    traces.rotateServiceCsvs(nc, log);

    // Health heartbeat (RTB-6); the body is in NetworkRunTraces (INC-4), and the
    // window anchors and last_health_write_s live in ctx.counters.
    traces.writeHealthRow(se.time, nc, ctx.counters, timing.loop_begin,
                          ura_on, raim_on, pl_on, ura_sigma0_ewma,
                          last_pivot, grade_metric_s);

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

    // The slip funnel opens (network/slip/SlipFunnel.hpp): the previous epoch's deferred
    // innovation-gate flags, and the retap-only drain in lockstep.
    networkSlipFunnelOpen(run, ep, ep.funnel, ep.events);

    // --- PASS A: per-station geometry + shared-state layout -------------
    ep.pass_a.modeled = std::vector<std::vector<PppObservation>>(stations.size());
    ep.pass_a.present = std::vector<bool>(stations.size(), false);
    // The pivot-first head, the PASS-A parallel tail and the slip scans live in
    // ApplicationNetworkAssembly.cpp.  networkPassAHead_ models the legacy pivot
    // (with its skip bookkeeping; SkipEpoch ends the epoch) and seeds the legacy
    // shared set; the SB_SLIP wall times the scan.
    if (networkPassAHead_(run, ep, ep.pass_a) == EpochAction::SkipEpoch)
        return LoopSignal::Next;

    networkSubBegin(run, SB_SLIP);   // block 1: PASS-A slip scans, pre-assembly (D0b)
    ++slip_qc.epoch_idx;   // monotonic processed-epoch index (card G gap arithmetic)
    networkPassASlipScan_(run, ep, ep.pass_a, ep.funnel, ep.events);
    networkSlipFunnelUnionRaw(run, ep, ep.funnel);   // the raw subset joins the funnel
    networkSubEnd(run, SB_SLIP);       // close block 1, slip scans (D0b)
    networkSubBegin(run, SB_PREASM);   // pre-assembly measurement build, post-slip → assembly wall (D0b)

    // The raw slip observables and the cross-station observability pre-filter live in
    // ApplicationNetworkAssembly.cpp; the writer-before-prune ordering contract is
    // structural inside networkAssemblyPrefilter_.
    networkAssemblyPrefilter_(run, ep, ep.pass_a);

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
    networkSignalIntegrity(run, ep, ep.pass_a, ep.funnel);
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
    networkMeasurementAdmission(run, ep, ep.pass_a, ep.funnel, ep.events);
    // The phase datum (network/solve/PhaseAnchorEpoch.cpp): the final shared phase-bias set
    // and the station that anchors each (sat,signal).  After the admission screen, whose
    // withholds prune the states an election could otherwise pin.
    networkPhaseAnchorEpoch(run, ep, ep.pass_a, ep.events);

    networkReceiverPhaseBiasAllocation(run, ep);   // eq34 RPB allocation (rule D3c); inert when eq34 is off

    // Shared-column registration, counts and estimability descriptors are in
    // ApplicationNetworkAssembly.cpp (networkAssemblyRegisterColumns_ below).  The
    // outputs are fresh per epoch; the empty-system skip keeps its bookkeeping inside
    // (SkipEpoch return), and the Next return below bypasses networkSubEnd(SB_PREASM).
    // est.beginEpoch() runs inside on Proceed.
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::BeforeColumns, ep.events);

    if (networkAssemblyRegisterColumns_(run, ep, ep.columns)
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
    networkSdEwlAr(run, ep);

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
    networkOraclePinEpoch(run, ep);

    // The bounded drop (RF-1 step 4, network/ar/AmbFloat.cpp): serially, after PASS A's
    // slip scans latched relevel.free_ambs and before the parallel PASS-B inject reads the carry.
    networkAmbFloatDropEpoch(run, ep, ep.events);
    // --- Exact coupling time update, per-epoch inputs (card CTU) ---------------
    // After the columns are registered (the layout the rows resolve against) and
    // before PASS B, whose parallel produce reads the result.  The rows read this
    // epoch's D2 fold decision, which card PRM phase 1b makes once here (after the
    // seam's level noise is set) and the fold, later in networkSolveGlue, reads too.
    // Inside the SB_PREASM bracket for the reason networkSdEwlAr gives.  No-op off.
    networkClockLevelRestoreRaise(run);   // card CTU G5: once, the first leg epoch after a warm resume
    // Card PRM phase 2 (network.handover_stransform_before_rows): a clock-pivot handover's
    // S-transform before the decision and PASS B, through the traversal of carried
    // references, so the fold decision, the exact update's prepare and the coupling rows
    // read the re-expressed carry; the solve glue's transform then does the bookkeeping.
    // After the restore raise, so the two compose in the order the solve glue applied
    // them.  No-op off (it clears its per-epoch record and returns).
    networkHandoverStransformBeforeRows(run, ep, ep.events);
    networkCarryDecide(run, ep);
    networkCouplingExactPrepare(run, ep);
    networkSubEnd(run, SB_PREASM);   // close pre-assembly; the assembly wall below is timed separately (D0b)

    // --- PASS B (in ApplicationNetworkAssembly.cpp): networkPassB_ owns the D0
    // assembly wall (a raw chrono pair; the sinks are passed by reference), the T5
    // sh-dev aging, the card-1d reinit history, the parallel produce loop (including
    // the coupling-injection cluster, whose row builder is filter/NetworkCoupling
    // since INC-7b), and the serial consume.  The failed / empty-epoch skip keeps its
    // bookkeeping inside (SkipEpoch return).
    ep.assembly.asm_res = std::vector<StationAssembly>(stations.size());
    // est-station index per station (the order addStation was called) so the
    // post-solve back-substitution can recover each present station's local
    // slant-iono block; -1 for stations not added this epoch.
    ep.assembly.est_index_of = std::vector<int>(stations.size(), -1);
    if (networkPassB_(run, ep, ep.pass_a, ep.assembly) == EpochAction::SkipEpoch)
        return LoopSignal::Next;
    // The block ahead of the DD build (D0b); until card NOM3 step EP1 it also set up the
    // carry-driver closure buildCarryDdPriors (the epoch function and the solve's replay
    // now call networkBuildCarryDdPriors directly).
    networkSubBegin(run, SB_GAUGEBUILD);


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
    networkGapResumeCensus(run, ep);

    networkSubEnd(run, SB_GAUGEBUILD);   // close the block ahead of the DD build (D0b)
    networkSubBegin(run, SB_DDBUILD);    // carry_driver.beginEpoch + networkBuildCarryDdPriors, W·Pᶻ·Wᵀ (D0b)
    networkCarryDriverBeginEpoch(run, ep);   // the z-store predict, once per epoch (carry_invariant)
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
    { CarryEigenScope es(carry_par, n_threads); networkBuildCarryDdPriors(run, ep, ep.dd_build); }  // parallel_carry: thread W·Pᶻ·Wᵀ
    networkSubEnd(run, SB_DDBUILD);      // close networkBuildCarryDdPriors, the card-4 DD-cov projection (D0b)
    networkSubBegin(run, SB_SOLVEGLUE);  // pre-solve prep + clock predict + solve + within-epoch re-solve (D0b)
    ep.solve = networkSolveGlue(run, ep, ep.ws, ep.assembly.est_index_of, ep.dd_build, ep.events);
    if (ep.solve.action == EpochAction::SkipEpoch) return LoopSignal::Next;
    networkSubEnd(run, SB_SOLVEGLUE);   // close solve glue; net excludes the internal solve/reduce (D0b)
    networkSubBegin(run, SB_INSTALL);   // carry_driver.installPosterior, DD posterior mean/cov readback (D0b)
    // Installs the DD posterior (mean and covariance) back into the carry from the
    // final accepted x_s and datum: the invariant DDs the next epoch projects forward.
    // It runs after acceptance (after the skip paths of the observability and σ̂0
    // withhold gates above), so a withheld or contaminated epoch never enters the
    // carry, and outside the within-epoch RAIM block, so it runs on every accepted
    // epoch, not only RAIM-armed ones.  A failure resets the carry (a safe cold
    // start).  No-op when off.
    networkInstallCarry(run, ep, ep.events);
    networkSubEnd(run, SB_INSTALL);     // close installPosterior, the card-4 posterior readback (D0b)
    networkSubBegin(run, SB_NETAR);     // producer-side network AR phase 2, log-only, off in the tier (D0b)
    ep.ar_fixes = networkProducerAr(run, ep);
    networkSubEnd(run, SB_NETAR);      // close producer AR / pre-emit tail (D0b)
    networkSubBegin(run, SB_SOLPREP);  // toSolutionEpoch + per-satellite discontinuity signalling (D0b)
    // Decision half of the gross-LOS screen (RF-3b B3,
    // docs/refound/RF-3_code_row_qc_2026-09-22.md §4).  It runs ahead of the emission,
    // so a declaration reaches the epoch it declares rather than the one after it.  The
    // response half stays at the post-harvest site below, where it must be.  It runs
    // inside this existing bracket rather than behind a new timer slot, because a new
    // slot would change the section-timing artifact for every run, armed or not (as
    // for networkSdEwlAr).  Unreachable unless network.gross_los_emit_exclude.
    networkGrossLosScreen(run, ep, GrossLosPhase::Decide, ep.events);
    // The fixed-cycle fold (card FCF, network/ar/FixedCycles.hpp) and the ledger of
    // fixed cycles it steps (network/ar/ArLattice.hpp, card AGT's lattice, moved here
    // from SB_NETAR at step FCF-3): the search's fixed L1/L2 pairs, or the pairs the ledger
    // implies, folded into a held solve beside the float one.  After the gross-LOS Decide,
    // so its declarations release this epoch, and inside this bracket for the reason given
    // above.  Under fold emit with an accepted held solve, solution prep and the emitter
    // read the held vector (x_emit); every other seam keeps the float x_s.  Unreachable
    // unless the fold is armed or network.ambiguity_resolution.trace_lattice.
    ep.fixed_cycles = networkFixedCycles(run, ep);
    ep.prep = networkSolPrep(run, ep, ep.events);
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
    networkPbHoldCloneUpdate(run, ep);
    networkSubBegin(run, SB_EMITSTORE); // block 7: re-base + store push + solution CSV (D0b)
    networkEmitStore(run, ep, ep.prep.sol);
    networkSubEnd(run, SB_EMITSTORE);   // close block 7: emit/store, order-sensitive, serial (D0b)
    ep.emitted = ctx.counters.n_emitted != n_emitted_before;
    networkSubBegin(run, SB_IONODIAG);  // block 4a: iono / ZWD / anchor-info viz traces (D0b)
    networkIonoDiag(run, ep);
    networkSubEnd(run, SB_IONODIAG);   // close block 4a, iono/ZWD/anchor traces (D0b)
    networkSubBegin(run, SB_DDFRAC);   // block 6: cross-station DD fractional-residual trace, B5 (D0b)
    networkDdFrac(run, ep);
    networkSubEnd(run, SB_DDFRAC);       // close block 6, dd-frac B5 (D0b)
    networkSubBegin(run, SB_SHDEV_VTEC); // block 4b: T5 SH-deviation carry KF + VTEC coefficient trace (D0b)
    networkShDevVtec(run, ep);
    networkSubEnd(run, SB_SHDEV_VTEC);  // close block 4b, T5 sh-dev + VTEC trace (D0b)
    networkSubBegin(run, SB_T1A);       // block 3: T1a SH health probe (D0b)
    networkShHealth(run, ep, ep.events);
    networkSubEnd(run, SB_T1A);        // close block 3, T1a health (D0b)
    networkSubBegin(run, SB_LEVELING); // block 5: phase-leveling carry harvest (D0b)
    networkLeveling(run, ep, ep.events);
    networkSubEnd(run, SB_LEVELING);   // close block 5, leveling (D0b)
    networkSubBegin(run, SB_RAIM);     // block 2: post-fit network RAIM + median-polish attribution (D0b)
    // RAIM pool re-host: `carry_warm` requires network.phase_leveling, so gating on
    // it alone would leave post-fit RAIM unreachable on every card-4 lineage that
    // leaves leveling off (armed at load, never executed).  ctx.knobs.raim_ci_on is
    // the re-hosted arming condition (already ANDed with raim_on and the resolved
    // carry_invariant).
    if (raim_on && (carry_warm || ctx.knobs.raim_ci_on)) {
        networkPostFitRaim(run, ep, ep.solve.handoff);
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
    networkGrossLosScreen(run, ep,
                          ctx.knobs.gl_emit_exclude ? GrossLosPhase::Act : GrossLosPhase::Full, ep.events);
    networkSubEnd(run, SB_RAIM);       // close block 2, RAIM (D0b)
    networkSubBegin(run, SB_CLOCKUPD); // block 8: two-state clock update + fault QC + retro-flag feed (D0b)
    networkClockQc(run, ep, ep.solve.handoff, ep.events);
    networkSubEnd(run, SB_CLOCKUPD);       // close block 8, clock update + retro feed (D0b)
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::AfterClockQc, ep.events);
    // Roster disagreement pass (network/assembly/SignalRoster.cpp, card SID phase 1).  It sits here
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
    networkSignalRosterAudit(run, ep);
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::RosterTail, ep.events);
    networkSubEnd(run, SB_ROSTER);
    // The frame diagnostics (SPEC_day129_frame_guards P1-3/P1-4) and the censuses run
    // before the stash/status block, because the epoch-status row written there reads
    // the post-fit sigma0 and the prefit scalars they fill in.  All off by default;
    // unarmed, each returns on its first line.
    networkSubBegin(run, SB_FRAMEDIAG);
    // Predict half of the block-Baarda census (BLOCKFORMAL): each family's σ̂₀
    // through the previous epoch is frozen and the per-epoch ladders are cleared
    // before the census's rider feeds this epoch's blocks.  Predict-then-update is
    // the D5 v2 contract: a block standardized by a calibration it contributed to can
    // standardize itself away.
    networkBlockBaardaPredict(run, ep);
    // Station-congruence predict half, with the same contract and reason: σ̂₀ through
    // the previous epoch is frozen before the census's rider feeds this epoch's
    // stations, so a station cannot standardize its own coordinate fault away.
    networkCongruencePredict(run, ep);
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::FrameDiag, ep.events);
    networkSubEnd(run, SB_FRAMEDIAG);
    networkSubBegin(run, SB_STASH_STATUS); // D2/C1/SH carry re-stash + VCE fold + epoch-status QC (D0b)
    networkStashStatus(run, ep, ep.events);
    // Card SMH: the clones re-harvested on the stash just written, and the clones of
    // the generations that started at this epoch's emit born on it.  No-op off.
    networkPbHoldCloneHarvest(run, ep);
    // The level monitor of card CTU reads the post-fit code and the block the stash
    // just wrote; with the bound raise armed it raises that block before the next
    // fold.  It runs only on a kept epoch, which is every epoch that reaches here.
    // No-op off.
    networkClockLevelMonitor(run, ep);
    // The measure-only instruments of this spine point (network/driver/EpochObservers.hpp).
    networkObserveEpoch(run, ep, NetworkObserverSlot::StashTail, ep.events);
    networkBufferedTraceDrains(run, ep);   // cards LGM and AIL: the buffered trace rows
    networkSubEnd(run, SB_STASH_STATUS);   // close the stash and epoch-status tail, before the readout (D0b)

    // Per-epoch section-timer readout, emitted-epoch path (D0 and D0b, network/driver/Run.cpp).
    networkSectionTimersReadout(run, ep, sec);

    // Periodic atomic snapshot (RTB-7(a)); the body is in StateSnapshotWrite.cpp
    // (INC-4).  It stays after the prev_week/prev_tow advance, so the snapshot
    // captures this epoch.
    networkWriteStateSnapshot(run, ep);
    return LoopSignal::Next;
}

} // namespace GnssProcessor
