/**
 * @file ApplicationNetworkSetup.cpp
 * @brief @internal Setup phases of Application::runNetwork: Phase-0 input
 *        validation, Phases 2-4, the owned product sink, the S-basis contract
 *        and the closed-loop consumer, split out of ApplicationNetwork.cpp
 *        (decomposition step INC-5).  Outputs land in NetworkSetupResult,
 *        filled in place; its header states the address-stability contract.
 *        Each function returns 0 on success; on a fatal setup error it logs the
 *        error and returns 1, which runNetwork returns unchanged.  The include
 *        block is the source TU's, copied whole (the INC-1 precedent); its
 *        pruning was left to the INC-12 cleanup and has not been done, so it
 *        lists unused headers.
 */

/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK network producer).
 *
 * This block is a copy of the file header of ApplicationNetwork.cpp; that file
 * holds the current version.
 *
 * runNetwork has its own translation unit, like the other per-mode pipelines
 * (ApplicationBaseline.cpp, ApplicationRealtime.cpp).  The network producer is
 * a joint multi-station estimator: per-station local normals are reduced onto a
 * shared satellite-parameter block by an exact Schur complement, which estimates
 * the estimable satellite clock and satellite phase bias under an explicitly
 * declared minimum-constraint S-basis.  The products are emitted in IGS-SSR
 * convention for the companion PPP_RTK_User consumer.
 *
 * See docs/SPEC_sbasis_realization_contract.md §F7 and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * ===========================================================================
 * The live epoch driver (W4-S2b)
 * ===========================================================================
 *
 * Each solution epoch runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  For each present station, PppModel::computeModels, then
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-satellite clock, per-(sat, signal) phase bias)
 *       behind the same usability gate the measurement build uses.  The datum
 *       (pivot) station is processed first, so its usable phase-signal set
 *       restricts the phase-bias columns to those backed by a pivot-receiver
 *       ambiguity pin.  The estimator's shared column layout is then registered:
 *       a clock for every satellite any station sees, and a phase bias for each
 *       signal in the pivot's set.
 *
 *   PASS B (assembly)  For each present station,
 *       MeasurementModel::computeMeasurementUpdate produces an undifferenced
 *       H / innovation / R block (no receiver-side differencing,
 *       DifferencingMode::None).  Its columns are split: the satellite clock (−1)
 *       and phase-bias (+1) columns scatter into the shared border A_s; the three
 *       receiver-position columns are dropped (coordinates are held fixed); the
 *       rest form the station's private nuisance block A_r.  The pivot station
 *       also folds tight-prior pins on its own ambiguities.  These pins are the
 *       local half of the S-basis, so the emitted phase bias carries that
 *       pivot-receiver ambiguity lump (Odijk Eq. 4).
 *
 *   Solve  NetworkSbasis builds the shared minimum-constraint datum (one
 *       satellite-clock pin per constellation); the null-space rank gate checks
 *       that the reduced shared system has exactly that deficiency; then
 *       NetworkEstimator::solve produces the shared estimate.
 *       NetworkProductEmitter serializes it into an owned SsrCorrectionStore,
 *       never the process-wide singleton the user side reads, which keeps the
 *       closed loop isolated.
 *
 *   Between epochs (D2)  The shared posterior (value, variance) is carried
 *       forward as a diagonal prior on the next epoch's shared border (dense
 *       with network.shared_carry_full_cov), inflated by the per-parameter
 *       process noise over the epoch gap.
 *
 * Base scope (V1): estimable satellite clock and phase bias, coordinates and
 * orbits fixed, GPS and Galileo, per-band uncombined, float (no network AR).
 * Satellite code bias, atmosphere products, the real-time branch, the GLONASS
 * FCN bias and network QC were planned as later slices (W6/W7/Phase 2); some
 * have landed since, among them satellite code-bias estimation (Phase 3b below)
 * and the broadcast a-priori of the real-time service mode (brdc_rts).  Each
 * station's coordinates must be known (they are held fixed) and its observation
 * RINEX path must be set explicitly in the config: the PathResolver network gate
 * is deferred, so resolveData() serves only products and broadcast navigation.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "network/emit/NetworkEmitRebase.hpp"   // emit-layer frame re-base (Card 4, M1 phase D)
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // combined-correction σ arithmetic (M4)
#include "products/NetworkStateSnapshot.hpp"   // atomic state snapshot and warm resume (RTB-7)
#include "products/LiveEphemerisStore.hpp"     // broadcast a-priori store for brdc_rts (Card 5, RTF-1)
#include "products/RinexNavParser.hpp"         // RINEX-nav → LiveEphemerisStore loader (Card 5, RTF-1)
#include "network/solve/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "network/solve/NetworkSbasis.hpp"
#include "network/solve/NetworkPivotHandover.hpp"   // pivot-handover datum-change detector (A1/A2)
#include "network/carry/NetworkCarryVce.hpp"        // LS-VCE carry-row variance-component monitor (M2)
#include "network/carry/NetworkInvariantCarryDriver.hpp"  // DD cycle-basis invariant carry (Card 4, M1 C3b)
#include "filter/StateKeySchema.hpp"
#include "network/driver/NetworkRunTypes.hpp"      // run-loop record types: NetStation, StationAssembly, carry PODs (INC-2)
#include "network/qc/NetworkRobustStats.hpp"   // median/MAD robust attribution helpers (INC-2)
#include "logging/CsvRotator.hpp"     // size-based rotation of the unbounded service CSVs (P1)
#include "network/diag/NetworkRunTraces.hpp" // trace-stream ownership, open logic, roster, FlagRate (INC-3)
#include "network/state/NetworkRunContext.hpp"    // ctx: knobs, Tier-1 carry state, service counters (INC-4)
#include "slip/MwSlipDetector.hpp"      // phase-leveling slip repair (T0, Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "network/slip/NetworkSlipQc.hpp"       // slip-QC pure helpers: ifElevMap, parseIfPairResetKey (INC-2)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"    // systemOfSat, letterOfSat, systemName, ObservableId, SignalId
#include "common/PppUtils.hpp"        // signal wavelengths for the DD fractional-cycle diagnostic (B5)
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads: Eigen is pinned to one thread inside the per-station OpenMP region, which avoids nested oversubscription and keeps the result deterministic
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
#include "network/driver/NetworkSetupResult.hpp"

// The run-end reports at the end of this file (card NOM step TD1b):
#include "config/GnssConfig.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include <spdlog/spdlog.h>
#include "network/driver/NetworkRun.hpp"
#include "network/state/NetworkRunContext.hpp"
#include "network/diag/NetworkRunTraces.hpp"

namespace GnssProcessor {

int Application::networkValidateInputs_() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    // --- Phase 0: validate inputs (explicit RINEX + fixed coordinates) ------
    // Every station needs an explicit observation RINEX path: the PathResolver
    // network gate (filling paths from the internal data tree by station name)
    // is deferred, because it would edit the resolve path shared with the other
    // modes in Application.cpp.  Coordinates are validated by
    // loadNetworkSection_ (every station needs a known position).
    for (const auto& s : nc.stations) {
        if (s.rinex_path.empty()) {
            log->error("Network: station '{}' has no observation rinex_path. V1 "
                       "requires an explicit network.stations[].rinex_path for "
                       "every station (PathResolver network auto-discovery is "
                       "deferred).", s.name);
            return 1;
        }
    }
    return 0;
}

int Application::networkSetup_(NetworkSetupResult& setup) {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }

    // --- Phase 2: multi-station RINEX ingestion + epoch alignment -----------
    // MultiRinexSync is driven by a BaselineConfig roster; adapt the network
    // stations into it with the clock-datum station as the "reference" so the
    // synced schedule walks its epochs (by_station includes the reference).
    auto& roster = setup.roster;
    roster.epoch_sync_tolerance_s = nc.epoch_sync_tolerance_s;
    bool pivot_found = false;
    for (const auto& s : nc.stations) {
        if (s.name == nc.clock_datum_station) {
            roster.reference = s;
            pivot_found = true;
        } else {
            roster.rovers.push_back(s);
        }
    }
    if (!pivot_found) {
        log->error("Network: clock_datum_station '{}' not in stations list",
                   nc.clock_datum_station);
        return 1;
    }

    // Closed-loop integer-recovery harness: a held-out consumer station joins the
    // sync roster only so that its observations are time-aligned with the network
    // schedule.  It is not one of the joint estimator's stations (built below);
    // it consumes the producer's emitted SSR products instead.
    const ClosedLoopConfig& cl = nc.closed_loop;
    setup.closed_loop_on = cl.enable && !cl.station.name.empty();
    const bool closed_loop_on = setup.closed_loop_on;

    // A-priori orbit/clock source (network.apriori_source).  sp3 (default): the
    // precise SP3/CLK cache is the a-priori, byte-identical to the producer before
    // the knob existed.  brdc_rts: the a-priori is broadcast ephemeris
    // (LiveEphemerisStore), the self-sufficient real-time service mode.  The flag is
    // threaded through Phase 3 (nav load and SP3-error lift), the producer-station
    // gate suppression (D1), the anchor-σ re-base (D6), the clock-IODE stamp and the
    // apriori_product_id.  Every use is guarded on it, so the sp3 path is textually
    // unchanged.  (Card 5, RTF-1)
    setup.brdc_rts =
        (config_.networkAprioriSource() == NetworkAprioriSource::BrdcRts);
    const bool brdc_rts = setup.brdc_rts;
    if (closed_loop_on) {
        roster.rovers.push_back(cl.station);
        log->info("Network: closed-loop consumer '{}' added to the sync roster "
                  "(held OUT of the joint solve)", cl.station.name);
    }

    setup.sync = std::make_unique<MultiRinexSync>(config_, roster, log_manager_,
                                                  *product_parser_);
    MultiRinexSync& sync = *setup.sync;

    // Clip each station's parse to the processing window.  The epoch loop in
    // runNetwork processes at most processing.max_duration_s of data; without the
    // clip, load() would parse (and synchronize() would copy) the full 24 h of every
    // station even when only the first minutes are used, which dominates wall time
    // at ~50 full-day stations.  The 60 s margin covers per-station first-epoch
    // jitter; max_duration_s <= 0 keeps full-file parsing.
    //
    // The parse window is anchored at the file start, but network.start_offset_hours
    // makes the epoch loop skip that span first, and the max-duration cutoff counts
    // from the first epoch past the offset.  The window therefore covers
    // [0, start_offset + max_duration + margin]; clipping to max_duration alone would
    // cut off the offset span and leave no epochs to solve.
    if (config_.getMaxDuration() > 0.0) {
        const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;
        const double span_s = start_offset_s + config_.getMaxDuration() + 60.0;
        sync.setParseWindowSeconds(span_s);
        if (start_offset_s > 0.0)
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each station "
                      "(start_offset {:.0f} s + max_duration_s {:.0f} s + 60 s margin)",
                      span_s, start_offset_s, config_.getMaxDuration());
        else
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each "
                      "station (max_duration_s {:.0f} s + 60 s margin)",
                      span_s, config_.getMaxDuration());
    }

    if (!sync.load()) {
        // Non-fatal: a station that failed to parse is excluded from the aligned
        // schedule (synchronize() skips it; the datum/pivot is checked below in
        // buildStation).  A large roster therefore proceeds when some stations'
        // observation files are missing or corrupt, which is expected when fetching
        // ~50 stations from a public archive.
        log->warn("Network: one or more station RINEX files failed to parse; "
                  "proceeding with the stations that loaded (the datum station "
                  "is required and is checked next).");
    }
    if (!sync.synchronize()) {
        log->error("Network: failed to synchronise epochs across stations");
        return 1;
    }
    // The network's single admission point for constellations.  The parser keeps every
    // system and the model admits an observation by its signal priority list, while each
    // station's inter-system bias is allocated by processing.active_systems, so an
    // inactive system with a priority list would enter the rows with no bias column.  The
    // loader accepts that combination with a warning; removing the inactive systems here,
    // before any reader of the synced epochs runs, makes processing.active_systems the one
    // admission rule.  (Card OWD, O4)
    {
        const std::map<std::string, std::size_t> removed =
            sync.retainSystems(config_.getActiveSystems());
        std::string active, what;
        std::size_t n_removed = 0;
        for (const std::string& sys : config_.getActiveSystems())
            active += (active.empty() ? "" : ", ") + sys;
        for (const auto& [sys, n] : removed) {
            what += fmt::format("{}{} {}", what.empty() ? "" : ", ", sys, n);
            n_removed += n;
        }
        log->info("Network (card OWD): admission by processing.active_systems [{}] — {} "
                  "observation(s) of inactive systems removed from the synced epochs{}{}.",
                  active, n_removed, what.empty() ? "" : ": ", what);
    }
    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Network: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether the station files overlap in time");
        return 1;
    }
    log->info("Network: multi-station sync complete — {} synchronised epochs", synced.size());
    // The processing window [window_t0_s, window_t1_s), with the epoch loop's own bounds:
    // the start-offset skip of runNetwork's fast-forward, then the max-duration cutoff
    // (without a cutoff the window ends 1 s after the last synced epoch).  The group-delay
    // latch takes the navigation records in force during the window, and the S contract
    // names the product files in force during it.  (Card OWD)
    setup.window_t0_s = TimeUtils::continuousGpsSeconds(synced.front().time)
                      + config_.networkStartOffsetHours() * 3600.0;
    setup.window_t1_s = config_.getMaxDuration() > 0.0
                      ? setup.window_t0_s + config_.getMaxDuration()
                      : TimeUtils::continuousGpsSeconds(synced.back().time) + 1.0;

    // Card HLT: broadcast satellite health on the network path (network.sat_health).  The
    // series is built once from the navigation file, a record taking force at its clock
    // epoch as in the O3 latch.  "exclude": a satellite's observations leave the synced
    // epochs at which the broadcast flags it, here beside the constellation door, so every
    // reader downstream sees it absent there (a return is the satellite's re-admission, as
    // after an eclipse).  "pivot": the clock-pivot election reads the series
    // (NetworkSolveGlue) and the observations stay.
    if (config_.networkSatHealth() != 0) {
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (card HLT): network.sat_health needs a broadcast navigation "
                       "file, but files.navigation_path is empty.  Set it, let PathResolver "
                       "auto-resolve a BRDC / _MN.rnx file covering the day, or set "
                       "network.sat_health to off.");
            return 1;
        }
        const std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        const auto& priorities = config_.getSignalPriorities();
        const auto gal = priorities.find(systemName(System::Galileo));
        int gal_mask = gal != priorities.end() ? NetworkSbasis::galileoHealthMask(gal->second) : 0;
        if (gal_mask == 0) gal_mask = 0x1FF;   // no Galileo band named: every signal's bits count
        setup.sat_health = NetworkSbasis::satHealthSeries(nav, gal_mask);
        if (setup.sat_health.sats.empty()) {
            log->error("Network (card HLT): parsed 0 usable broadcast records from '{}', so "
                       "network.sat_health has no health to read.", nav_path.string());
            return 1;
        }
        // The satellites flagged at some time inside the window, with the flagged spans.
        const double t0 = setup.window_t0_s;
        const double t1 = setup.window_t1_s;
        std::set<std::string> flagged_sats;
        std::string flagged;
        int n_edges = 0;
        for (const auto& [sat, steps] : setup.sat_health.sats) {
            std::string spans;
            for (std::size_t i = 0; i < steps.size(); ++i) {
                if (i > 0 && steps[i].t_gps_s > t0 && steps[i].t_gps_s < t1) ++n_edges;
                if (!steps[i].unhealthy) continue;
                // The first step's status also holds before its time.
                const double a = i == 0 ? t0 : std::max(t0, steps[i].t_gps_s);
                const double b = i + 1 < steps.size() ? std::min(t1, steps[i + 1].t_gps_s) : t1;
                if (b <= a) continue;
                spans += (spans.empty() ? "" : ", ")
                       + ((a == t0 && b == t1)
                              ? std::string("the whole window")
                              : fmt::format("TOW {:.0f} to {:.0f}", std::fmod(a, 604800.0),
                                            std::fmod(b, 604800.0)));
            }
            if (spans.empty()) continue;
            flagged_sats.insert(sat);
            flagged += fmt::format("{}{} ({})", flagged.empty() ? "" : "; ", sat, spans);
        }
        log->info("Network (card HLT): broadcast health from '{}' ({} record(s), {} unhealthy "
                  "under the band mask, Galileo mask 0x{:03X}): {} satellite(s) flagged inside "
                  "the window [TOW {:.1f}, TOW {:.1f}){}{}; {} status change(s) inside it.  "
                  "Mode '{}'.",
                  nav_path.filename().string(), setup.sat_health.records,
                  setup.sat_health.records_unhealthy, gal_mask, flagged_sats.size(),
                  std::fmod(t0, 604800.0), std::fmod(t1, 604800.0),
                  flagged.empty() ? "" : ": ", flagged, n_edges,
                  config_.networkSatHealth() == 1 ? "pivot" : "exclude");
        const std::string& declared = config_.getNetworkConfig().clock_datum_satellite;
        if (!declared.empty() && flagged_sats.count(declared))
            log->warn("Network (card HLT): the declared clock pivot {} "
                      "(network.clock_datum_satellite) is flagged unhealthy inside the window; "
                      "the declaration is kept.", declared);
        if (config_.networkSatHealth() == 2) {
            const std::size_t n_removed = sync.removeObservationsIf(
                [&](const GnssTime& t, const RinexObservation& o) {
                    const double ts = TimeUtils::continuousGpsSeconds(t);
                    if (!setup.sat_health.unhealthyAt(o.satellite, ts)) return false;
                    if (ts >= t0 && ts < t1) ++setup.sat_health_removed[o.satellite];
                    return true;
                });
            long long in_window = 0;
            std::string by_sat;
            for (const auto& [sat, k] : setup.sat_health_removed) {
                in_window += k;
                by_sat += fmt::format("{}{} {}", by_sat.empty() ? "" : ", ", sat, k);
            }
            log->info("Network (card HLT): the health door removed {} observation(s) of flagged "
                      "satellites from the synced epochs, {} inside the window{}{}.",
                      n_removed, in_window, by_sat.empty() ? "" : ": ", by_sat);
        }
    }

    // --- Phase 3: products + cache for geometry -----------------------------
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Network: failed to initialise product cache — no broadcast nav / orbits");
        return 1;
    }
    log->info("Network: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    // Staged broadcast nav records and the feed cursor for the progressive
    // LiveEphemerisStore insertion in the epoch loop (brdc_rts only; empty
    // otherwise).  A record's availability is its toc on the GPS-continuous scale
    // (a BDS toc is native BDT and is shifted to the GPS scale, as
    // LiveEphemerisStore does for toe).  The staging sort here and the epoch-loop
    // feed both call navAvailabilityGpsSeconds, one inline function in
    // app/NetworkSetupResult.hpp, so their orders cannot drift apart.
    // (Card 8, replay parity; INC-5)
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    if (brdc_rts) {
        // The a-priori is broadcast ephemeris, not SP3/CLK.  The RINEX navigation file
        // is parsed here and its records are fed, from the epoch loop, into the shared
        // LiveEphemerisStore, the store the real-time engine fills from RTCM
        // 1019/1042/1044/1046, so replay resolves satellite orbit and clock through the
        // same code path as a live run (frame consistency, conventions §1 / plan D4).
        // Geometry::getSatellitePV falls back to this store when no SP3 is loaded, and
        // GnssCorrections::getSatelliteClockCorrection supplies the broadcast SV clock
        // (marked broadcast-grade) when no precise CLK covers the epoch, which is the
        // broadcast a-priori this mode needs.  (Card 5, RTF-1)
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (brdc_rts): network.apriori_source = brdc_rts requires a "
                       "broadcast navigation file, but files.navigation_path is empty. "
                       "Set it, or let PathResolver auto-resolve a BRDC / _MN.rnx file "
                       "covering the observation day.");
            return 1;
        }
        // Parse first so a per-constellation loaded-satellite summary is available.
        //
        // The whole day is not inserted at once.  LiveEphemerisStore keeps only the
        // current and previous record per satellite (live streaming semantics, which
        // Cards 3/6/7 pin), so a full-file load would leave each satellite holding its
        // last two records of the day; at any processing epoch more than a fit window
        // (~4 h GPS, ~2 h GAL/BDS) before the end of the day, the staleness guard would
        // then reject every satellite and the producer would degenerate to
        // n_present = 0.  The records are therefore staged, sorted by availability, and
        // fed to the store as the epoch loop advances, so the store's current/previous
        // evolve as they would on a live nav stream.  Availability is the GPS-scale toc:
        // the file carries no receipt stamp (RinexNavParser sets received_time = {0,0}),
        // and toc is the closest file-derivable stand-in for "a live receiver holds this
        // record".  Live GPS broadcasts a data set from about toe − 2 h, so the toc proxy
        // is conservative by up to one fit half-window: records roll in slightly later
        // than live, never early.  (Card 8, replay parity, plan D4)
        std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        std::map<char, std::set<std::string>> nav_by_const;
        for (const auto& e : nav)
            if (!e.satellite.empty())
                nav_by_const[letterOfSat(e.satellite)].insert(e.satellite);
        if (nav.empty()) {
            log->error("Network (brdc_rts): parsed 0 usable broadcast ephemerides from '{}'. "
                       "Cannot form a broadcast a-priori — check the file is a "
                       "RINEX 3/4 navigation file covering the observation day.",
                       nav_path.string());
            return 1;
        }
        std::stable_sort(nav.begin(), nav.end(),
                         [&](const BroadcastEphemeris& a, const BroadcastEphemeris& b) {
                             return navAvailabilityGpsSeconds(a) < navAvailabilityGpsSeconds(b);
                         });
        brdc_nav_staged = std::move(nav);
        {
            std::size_t n_sats = 0;
            std::string summary;
            for (const auto& kv : nav_by_const) {
                n_sats += kv.second.size();
                summary += fmt::format("{}{}={}", summary.empty() ? "" : " ",
                                       kv.first, kv.second.size());
            }
            log->info("Network (brdc_rts): broadcast a-priori staged from '{}' — {} records, "
                      "{} satellites [{}]; fed to LiveEphemerisStore progressively by toc "
                      "(live-arrival replay).", nav_path.filename().string(),
                      brdc_nav_staged.size(), n_sats, summary);
        }
        // Any precise product loaded in brdc_rts is an unsupported combination and is
        // refused rather than masked (to revisit when the RTS-orbit seam, plan D9, lands).
        // The orbit cache and the clock cache must both be empty: a precise SP3 shadows
        // the broadcast a-priori through getSatellitePV node interpolation, and a precise
        // CLK shadows the broadcast clock in getSatelliteClockCorrection (interpolateClock
        // takes precedence over the broadcast fallback for covered epochs, giving
        // used_broadcast = false, not broadcast-grade).  Either one alone splits the
        // orbit/clock frame with nothing to flag it: the estimated C0 becomes a delta
        // against the wrong baseline while its stamped IODE still names the broadcast
        // ephemeris, so the consumer reconstructs brdc(iode) + C0 and is wrong at the
        // metre level.  The gate therefore tests the orbit count or the clock count; the
        // orbit count alone would pass a CLK-without-SP3 product set.
        if (product_cache.getOrbitCount() > 0 || product_cache.getClockCount() > 0) {
            log->error("Network (brdc_rts): precise products are loaded alongside "
                       "apriori_source = brdc_rts ({} SP3 orbit node(s), {} CLK sample(s)). This "
                       "combination is not supported — a precise orbit/clock would shadow the "
                       "broadcast a-priori (a silent orbit/clock frame split). Remove the SP3/CLK "
                       "product paths, or use apriori_source = sp3.",
                       product_cache.getOrbitCount(), product_cache.getClockCount());
            return 1;
        }
    } else if (product_cache.getOrbitCount() == 0) {
        // sp3 mode (default): the precise orbit cache is the a-priori, so it must hold orbits.
        log->error("Network: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path / products to cover the observation day.");
        return 1;
    }
    // Card NBP (network.apriori_source sp3_brdc_clock): SP3 orbits (required by the branch
    // above) and the broadcast clock of the navigation file, with no CLK product.  A loaded
    // CLK would shadow the broadcast clock silently (interpolateClock takes precedence in
    // getSatelliteClockCorrection), so it is refused, as brdc_rts refuses one.  The selector
    // is built here for every GPS satellite of the file, from 00:00 of the observation day
    // (a fixed origin: the level is an ignition draw, and a run-start origin would change
    // every satellite's level by its first step), so a warm resume recomputes the same
    // table.  GPS only: Galileo's I/NAV and F/NAV records carry different clock pairs and
    // the transmission-time rule alone would alternate between them.
    if (config_.networkAprioriBrdcClock()) {
        if (product_cache.getClockCount() > 0) {
            log->error("Network (card NBP): {} CLK sample(s) are loaded under "
                       "network.apriori_source sp3_brdc_clock.  A CLK product would shadow the "
                       "broadcast clock (a silent clock frame split); use a data root without "
                       "CLK files (e.g. runs/_scl_data_nbp) or apriori_source sp3.",
                       product_cache.getClockCount());
            return 1;
        }
        for (const std::string& sys : config_.getActiveSystems()) {
            if (sys != systemName(System::GPS)) {
                log->error("Network (card NBP): network.apriori_source sp3_brdc_clock is GPS-only "
                           "(processing.active_systems names '{}').", sys);
                return 1;
            }
        }
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (card NBP): network.apriori_source sp3_brdc_clock requires a "
                       "broadcast navigation file (files.navigation_path is empty).");
            return 1;
        }
        std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        nav.erase(std::remove_if(nav.begin(), nav.end(),
                                 [](const BroadcastEphemeris& e) { return e.system != System::GPS; }),
                  nav.end());
        if (nav.empty()) {
            log->error("Network (card NBP): parsed 0 GPS broadcast records from '{}'.",
                       nav_path.string());
            return 1;
        }
        const double origin_s = BrdcClockApriori::dayOrigin(setup.window_t0_s);
        setup.brdc_apriori = std::make_unique<BrdcClockApriori>(
            std::move(nav), origin_s, setup.window_t1_s, BrdcSelectRule::Ttom);
        const auto& bs = setup.brdc_apriori->stats();
        log->info("Network (card NBP): native broadcast clock a priori from '{}' (rule ttom): "
                  "{} GPS records, {} selectable (healthy, with a transmission time), {} "
                  "unhealthy, {} without a transmission time; {} satellites; offset table from "
                  "tow {:.0f} to {:.0f}: {} cut-overs, O range [{:.4f}, {:.4f}] m.",
                  nav_path.filename().string(), bs.n_records, bs.n_staged, bs.n_unhealthy,
                  bs.n_no_ttom, bs.n_sats, std::fmod(bs.origin_s, 604800.0),
                  std::fmod(bs.end_s, 604800.0), bs.n_switches, bs.offset_min_m,
                  bs.offset_max_m);
        if (bs.n_staged == 0) {
            log->error("Network (card NBP): no selectable GPS broadcast record in '{}'.",
                       nav_path.string());
            return 1;
        }
    }

    // --- Phase 3b: ionosphere and code-bias datum inputs (W6) ----------------
    // The satellite DCB (geometry-free code bias) is co-linear with each station's
    // free per-(rx,sat) slant ionosphere until a constraint or a datum imposes
    // between-satellite structure; without one the satellite DCBs stay in the null
    // space and the rank gate withholds every epoch.  The branches below are
    // exclusive and are tested in this order:
    //   - network.estimate_iono_sh: the network co-estimates its own SH-VTEC field
    //     and loads no GIM.
    //   - network.estimate_sat_code_bias with network.w6_iono_constraint = none
    //     (self-contained posture): no ionosphere constraint; the sum-to-zero rows
    //     carry the code-bias datum.
    //   - network.estimate_sat_code_bias otherwise (legacy W6): the IONEX GIM is
    //     loaded and networkAppendGimIonoRows_ folds a common-mode-free
    //     pseudo-observation per station, so a missing GIM is a hard error.
    // The external GIM and the SH field are therefore never both applied.
    auto& sh_model = setup.sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    if (config_.estimateNetworkIonoSh()) {
        sh_model = std::make_unique<SphericalHarmonicIonosphere>(shc.max_degree);
        log->info("Network (SH-iono): co-estimating own global SH-VTEC field — degree {} "
                  "({} coeffs), shell {:.0f} km, {} longitude; external GIM NOT loaded.",
                  sh_model->maxDegree(), sh_model->numCoeffs(), shc.layer_height_km,
                  shc.sun_fixed ? "sun-fixed" : "geographic");
        // Second thin shell of the dual-layer field (Iono-2, mechanism A).
        if (shc.layer2_max_degree > 0) {
            setup.sh_model2 =
                std::make_unique<SphericalHarmonicIonosphere>(shc.layer2_max_degree);
            log->info("Network (Iono-2): dual-layer field — second shell degree {} "
                      "({} coeffs) at {:.0f} km; pin rows sum both shells.",
                      setup.sh_model2->maxDegree(), setup.sh_model2->numCoeffs(),
                      shc.layer2_height_km);
        }
        // Echo the a₀₀ physical band that the per-epoch divergence warning uses (T1a).
        if (shc.a00_warn_low_tecu < shc.a00_warn_high_tecu)
            log->info("Network (SH-iono): a₀₀ physical band [{:.1f}, {:.1f}] TECU — a per-epoch "
                      "WARN fires if the estimated mean VTEC leaves it (field-divergence probe).",
                      shc.a00_warn_low_tecu, shc.a00_warn_high_tecu);
        else
            log->info("Network (SH-iono): a₀₀ physical-band WARN disabled (low ≥ high).");
    } else if (config_.estimateNetworkSatCodeBias()
               && config_.getNetworkConfig().w6_iono_constraint
                      == NetworkW6IonoConstraint::None) {
        // Self-contained W6 posture (RF-0b; docs/refound/RF-0_baseline_2026-09-14.md
        // Finding 2).  It leaves each satellite's geometry-free bias free and lets the
        // per-(constellation, signal) sum-to-zero rows realize the IGS convention, which
        // makes the posture reachable under processing.frequency_mode = dual.
        //
        // No GIM is loaded and no error is raised.  `gim_` stays null, so
        // networkAppendGimIonoRows_ returns immediately and the per-LOS slant ionosphere
        // floats on its own; the loader has already forced
        // estimation.ionosphere_weighting off, so no ionosphere constraint is left armed.
        log->info("Network (W6/no-constraint, RF-0b): NO ionosphere constraint and NO external "
                  "GIM — free per-LOS slant delays, with the satellite code-bias datum carried "
                  "by the per-satellite ionosphere-free pin plus the per-(constellation, signal) "
                  "SUM-TO-ZERO rows (IGS Sum_s DCB^s = 0).  The geometry-free lump is absorbed "
                  "by the ionosphere, so the emitted ionosphere and code biases must be applied "
                  "by a consumer AS A PAIR.  The between-satellite geometry-free split is "
                  "PRIOR-determined here, not data-determined: check the null-space gate's "
                  "observed-vs-declared and the run-end W6-IONO-NONE counter.");
        // --- Latch the broadcast group delays once, for the GF pin's RHS (RF-0c) ---
        // The navigation message is signal in space, not an external product, so reading
        // it here does not reintroduce the external-data dependence that RF-0b removed.
        // It is parsed independently of `brdc_rts` (that mode concerns the orbit/clock
        // a-priori; this is a datum value) and latched: T_GD is a hardware constant the
        // control segment updates rarely, and a datum that moved mid-run would step the
        // emitted code biases and the estimable ionosphere together with nothing on the
        // wire declaring it.  The per-satellite spread across the navigation file's
        // records is measured and reported, so a satellite whose T_GD changed is visible
        // instead of being averaged away.
        if (config_.getNetworkConfig().sat_gf_datum == NetworkSatGfDatum::BroadcastTgd) {
            const std::filesystem::path nav_path = config_.getNavigationPath();
            if (nav_path.empty()) {
                log->error("Network (RF-0c): network.sat_gf_datum = broadcast_tgd needs a "
                           "broadcast navigation file for the group delays, but "
                           "files.navigation_path is empty.  Set it, let PathResolver "
                           "auto-resolve a BRDC / _MN.rnx covering the day, or use "
                           "sat_gf_datum = zero (which needs no navigation data).");
                return 1;
            }
            const std::vector<BroadcastEphemeris> nav =
                RinexNavParser::parse(nav_path.string());
            // Latch the value in force during the processing window, not the file's first
            // record: the first record is the value in force when the file starts, which is
            // right only for a run that starts there.  The rule and its reasons are in
            // NetworkSbasis::latchBroadcastGroupDelays, beside the pair rule it feeds.
            // (Card OWD, O3)
            const NetworkSbasis::GroupDelayLatch latch = NetworkSbasis::latchBroadcastGroupDelays(
                nav, setup.window_t0_s, setup.window_t1_s);
            if (latch.sats.empty()) {
                log->error("Network (RF-0c): parsed 0 usable broadcast ephemerides from "
                           "'{}' — no group delays, so every satellite's geometry-free "
                           "datum would silently fall back to zero and the run would claim "
                           "a physical datum it does not have.  Check the file is a RINEX "
                           "3/4 navigation file covering the observation day, or use "
                           "sat_gf_datum = zero.", nav_path.string());
                return 1;
            }
            double worst_spread_m = 0.0;
            std::string worst_sat, zeros, changed, all_unhealthy;
            int n_zero = 0, n_changed = 0;
            for (const auto& [sat, g] : latch.sats) {
                setup.sat_gf_target_m[sat] = Constants::SPEED_OF_LIGHT * g.value_s;
                if (g.value_s == 0.0) {
                    ++n_zero;
                    zeros += (zeros.empty() ? "" : " ") + sat;
                }
                const double sp = Constants::SPEED_OF_LIGHT * g.spread_file_s;
                if (sp > worst_spread_m) { worst_spread_m = sp; worst_sat = sat; }
                if (g.value_s != g.first_s) {
                    ++n_changed;
                    changed += fmt::format("{}{} {:+.4f} m", changed.empty() ? "" : ", ", sat,
                                           Constants::SPEED_OF_LIGHT * (g.value_s - g.first_s));
                }
                if (g.reason == NetworkSbasis::GroupDelayLatchReason::AllUnhealthy
                    || g.reason == NetworkSbasis::GroupDelayLatchReason::AllUnhealthyInForceAtStart)
                    all_unhealthy += (all_unhealthy.empty() ? "" : " ") + sat;
            }
            log->info("Network (RF-0c, card OWD): latched broadcast group delays for {} "
                      "satellite(s) from '{}' ({} record(s) scanned) over the processing window "
                      "[TOW {:.1f}, TOW {:.1f}): the low median of each satellite's healthy "
                      "records whose clock epoch lies in the window, Galileo's F/NAV (E1, E5a) "
                      "records preferred.  Satellites: {} on the window rule, {} on the healthy "
                      "record in force at the window start, {} with no healthy record at all "
                      "(latched by the same rule over all their records: {}).  Records set "
                      "aside: {} unhealthy, {} Galileo I/NAV, {} outside the window.  {} "
                      "satellite(s) latched at exactly 0 ({}; a real value, one broadcast step "
                      "is 0.233 ns).  {} satellite(s) differ from the file's first record, the "
                      "pre-OWD latch{}{}.  Largest within-day |c*dT_GD| {:.4f} m{}; one value "
                      "per run, so the datum cannot churn.",
                      setup.sat_gf_target_m.size(), nav_path.filename().string(), nav.size(),
                      std::fmod(setup.window_t0_s, 604800.0),
                      std::fmod(setup.window_t1_s, 604800.0),
                      latch.n_window, latch.n_in_force, latch.n_all_unhealthy,
                      all_unhealthy.empty() ? "none" : all_unhealthy,
                      latch.dropped_unhealthy, latch.dropped_inav, latch.outside_window,
                      n_zero, zeros.empty() ? "none" : zeros, n_changed,
                      changed.empty() ? "" : ": ", changed, worst_spread_m,
                      worst_sat.empty() ? std::string{}
                                        : fmt::format(" (on {})", worst_sat));
        }
    } else if (config_.estimateNetworkSatCodeBias()) {
        // A second guard (Card 5): the external-GIM code-bias path is post-processing-only
        // (a GIM is a daily product, unavailable to the self-sufficient brdc_rts service).
        // The load-time validation (Card 1) rejects brdc_rts + estimate_sat_code_bias +
        // !estimate_iono_sh as a hard error, so reaching this branch in brdc_rts means that
        // check was bypassed; refuse rather than load a GIM the service cannot have.
        if (brdc_rts) {
            log->error("Network (brdc_rts): reached the external-GIM code-bias path, which is "
                       "post-processing-only. Set network.estimate_iono_sh = true to co-estimate "
                       "the ionosphere field, or disable network.estimate_sat_code_bias "
                       "(this combination should have been rejected at config load).");
            return 1;
        }
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log->error("Network (W6): network.estimate_sat_code_bias is on but no usable IONEX "
                       "GIM was resolved (looked for a .INX under the date's products/). The "
                       "satellite code bias is unobservable without the GIM ionosphere "
                       "constraint — supply a GIM (same analysis centre as the orbits/clocks for "
                       "bias consistency), enable network.estimate_iono_sh to co-estimate the "
                       "field instead, or disable estimate_sat_code_bias.");
            return 1;
        }
        log->info("Network (W6): GIM loaded for the satellite code-bias ionosphere constraint "
                  "(common-mode-free per-constellation weighting).");
    }

    // --- Network-wide signal coverage gate (network.signal_min_station_fraction) ---
    // A RINEX code enters the solve only if enough of the network tracks it, so a
    // sparse channel (e.g. GPS L2S, ~13% of stations) is dropped at every station
    // instead of being mixed in only at the stations that track it, which is what
    // the per-station channel_mode does.  Every retained signal's satellite bias is
    // then estimated from a dense, consistent station set.  Coverage is tallied over
    // the synced epochs: a station tracks a code if it reports it for any satellite
    // in any epoch, and coverage = (#stations tracking the code) / (#stations
    // observing that constellation).  Off (0) leaves the allowlist empty, so every
    // tracked channel is admitted (byte-identical to the ungated producer).
    //
    // Two independent gates share one coverage tally.  signal_min_station_fraction
    // drops a sparse code (PppModel::setSignalAllowlist).
    // network.phase_bias_min_station_fraction floats it instead: the observation is
    // kept but its satellite phase-bias state is withheld
    // (MeasurementModel::setPhaseBiasSignalAllowlist), so the float ambiguity absorbs
    // the bias and the signal is not AR-eligible.  Both default to 0 (empty
    // allowlists, byte-identical).
    std::map<std::string, std::set<std::string>> signal_allowlist;       // drop gate (code and phase)
    std::map<std::string, std::set<std::string>> phase_bias_allowlist;   // float gate (phase only)
    const double sig_cover_min = config_.networkSignalMinStationFraction();
    const double pb_cover_min  = config_.networkPhaseBiasMinStationFraction();
    // Under the all-channel widening the tally is reported even with the gate at 0:
    // "0 = every tracked channel admitted" is a statement about which channels
    // entered the solve, and the run log has to make it.  (Card SID phase 6b, R7)
    const bool chan_all_tally = config_.networkChannelModeAll();
    if (sig_cover_min > 0.0 || pb_cover_min > 0.0 || chan_all_tally) {
        std::map<std::string, std::set<std::pair<std::string, std::string>>> sta_tracks; // station -> {(system, code)}
        for (const auto& se : synced) {
            for (const auto& [sta, sat_obs] : se.by_station) {
                if (sat_obs.empty()) continue;
                auto& trk = sta_tracks[sta];
                for (const auto& ob : sat_obs) {
                    if (ob.system == System::Unknown) continue;
                    const std::string sys = systemName(ob.system);
                    for (const auto& [code, val] : ob.measurements) {
                        (void)val;
                        const ObsType t = obsTypeOf(code);
                        if (t != ObsType::Code && t != ObsType::Phase)
                            continue;   // code + phase channels only
                        trk.insert({sys, code});
                    }
                }
            }
        }
        std::map<std::string, int> sys_n;                          // system -> #stations observing it
        std::map<std::pair<std::string, std::string>, int> code_n; // (system, code) -> #stations tracking it
        for (const auto& [sta, trk] : sta_tracks) {
            (void)sta;
            std::set<std::string> sys_here;
            for (const auto& sc : trk) { ++code_n[sc]; sys_here.insert(sc.first); }
            for (const auto& sys : sys_here) ++sys_n[sys];
        }
        for (const auto& [key, n] : code_n) {
            if (sys_n[key.first] <= 0) continue;
            const double frac = static_cast<double>(n) / sys_n[key.first];
            if (sig_cover_min > 0.0 && frac >= sig_cover_min)
                signal_allowlist[key.first].insert(key.second);
            // The float gate applies to phase codes only (it withholds phase-bias states).
            if (pb_cover_min > 0.0 && obsTypeOf(key.second) == ObsType::Phase
                && frac >= pb_cover_min)
                phase_bias_allowlist[key.first].insert(key.second);
        }
        auto reportGate = [&](const char* label, double thr,
                              const std::map<std::string, std::set<std::string>>& allow,
                              bool phase_only, bool force = false) {
            if (thr <= 0.0 && !force) return;
            for (const auto& [sys, ns] : sys_n) {
                std::set<std::string> codes;
                for (const auto& [key, cnt] : code_n) {
                    (void)cnt;
                    if (key.first != sys) continue;
                    if (phase_only && obsTypeOf(key.second) != ObsType::Phase) continue;
                    codes.insert(key.second);
                }
                std::string keep, gate;
                auto ait = allow.find(sys);
                for (const auto& c : codes) {
                    const std::string tag = fmt::format("{}({:.0f}%)", c, 100.0 * code_n[{sys, c}] / ns);
                    // A threshold of 0 leaves the allow-list empty, which the producer
                    // reads as "admit every tracked channel"; without this clause the
                    // forced tally (card SID phase 6b) would print the whole list under
                    // GATE, the opposite of what the run does.
                    const bool admitted =
                        (thr <= 0.0) ? true : (ait != allow.end() && ait->second.count(c));
                    std::string& tgt = admitted ? keep : gate;
                    tgt += (tgt.empty() ? "" : " ") + tag;
                }
                log->info("Network {} gate [{}≥{:.0f}%]: {} stations observe {} — "
                          "KEEP [{}] | GATE [{}]", label, sys, 100.0 * thr, ns, sys, keep, gate);
            }
        };
        reportGate("signal-coverage (drop)", sig_cover_min, signal_allowlist, false,
                   chan_all_tally);
        reportGate("phase-bias (float)",     pb_cover_min,  phase_bias_allowlist, true);
        if (chan_all_tally && sig_cover_min <= 0.0)
            log->info("Network (card SID phase 6b): the coverage gate "
                      "network.signal_min_station_fraction is 0, so EVERY tracked "
                      "channel above is admitted and the widening models all of them.  "
                      "The KEEP list is the whole tally and the GATE list is empty by "
                      "construction; the per-channel percentages are what a nonzero "
                      "gate would cut against.");
    }

    // --- Phase 4: per-station Kalman resources (pivot at index 0) -----------
    // Each station gets a single-receiver StateManager (coordinates fixed at the
    // configured ECEF, or at its ARP reduction under network.station_marker_to_arp),
    // a PppModel bound to its RinexParser, and a MeasurementModel.  The pivot is
    // built first so PASS A can seed its phase-signal set before the others.
    auto& stations = setup.stations;
    // Stations whose RINEX receiver antenna matched no loaded ATX entry (zero receiver
    // PCO/PCV applied).  Collected during buildStation for a single startup summary
    // warning; the per-observation site logs only at trace level.
    std::vector<std::string> unmatched_antenna_stations;
    // Tier 1 (the always-on radome→NONE fallback): stations matched through the
    // type-preserving, radome-agnostic NONE calibration (mm-level) instead of an
    // exact (type, radome) entry.  Reported once.
    std::vector<std::string> radome_fallback_stations;
    // Tier 2 (network.exclude_unmodeled_antenna_stations): stations dropped from
    // the network because their receiver antenna type is unknown to the loaded
    // ANTEX even after the radome→NONE fallback (unmodelled receiver PCO/PCV).
    std::vector<std::string> excluded_unmodeled_antenna_stations;
    // Per active constellation with a priority list, the route each station's
    // receiver correction takes on the two lowest bands of that list ("exact",
    // "same_system", "cross_system", "none"), collected for one summary line:
    // system -> route -> station names.  Filled only when a constellation other than
    // GPS is active, so a GPS-only setup log is unchanged.  (Card GAL, R6)
    const bool atx_src_on = networkMultiGnssActive(config_);
    std::map<std::string, std::map<std::string, std::vector<std::string>>> atx_src_by_sys;
    // --- Marker → ARP roster reduction (card UPO) ----------------------------
    // Marker → antenna-reference-point reduction of the roster's configured
    // coordinates (docs/refound/UPO_consumer_up_offset_2026-09-22.md §4).  The
    // eccentricities are read in both knob states (standing invariant #4: a run
    // with the knob off still reports the size of the error it does not correct);
    // the reduction is applied only when network.station_marker_to_arp is armed.
    const bool marker_to_arp = config_.networkStationMarkerToArp();
    int    n_ecc_stations   = 0;        // roster stations with a nonzero header eccentricity
    int    n_ecc_reduced    = 0;        // of those, the ones actually moved
    int    n_ecc_over_1cm   = 0;        // |H|, |E| or |N| above 0.01 m
    double ecc_max_h        = 0.0;      // largest |H| on the roster (signed value kept)
    double ecc_max_abs      = 0.0;      // largest |H|, |E| or |N| on the roster
    std::string ecc_max_h_station;
    std::vector<std::string> no_ecc_record_stations;  // header carried no record at all
    // buildStation outcome: Built (pushed to `stations`), ExcludedUnmodeledAntenna
    // (tier-2 policy drop: counted, not built), or Failed (parser/load error).
    enum class StationBuild { Built, ExcludedUnmodeledAntenna, Failed };
    auto buildStation = [&](const StationConfig& cfg, bool is_pivot) -> StationBuild {
        const RinexParser* parser = sync.parserOf(cfg.name);
        if (!parser) {
            log->error("Network: station '{}' parser unavailable", cfg.name);
            return StationBuild::Failed;
        }
        NetStation st;
        st.name     = cfg.name;
        st.ecef      = Eigen::Vector3d(cfg.position_ecef.data());
        // `position_ecef` is a published coordinate (IGS SSC/SINEX, NGS SIF, a
        // surveyed RINEX header) and therefore references the monument, while the
        // observations refer to the antenna and the ATX phase-centre model is defined
        // relative to the antenna reference point.  The reduction happens here, once,
        // before st.ecef reaches the state vector (x0 below), the PppModel, the
        // MeasurementModel, st.pstate or any geometry, through the same
        // Coordinates::markerToArp that the held-out consumer's truth uses.
        // markerToArp is an exact no-op at or below a 1e-6 m eccentricity, so a
        // 0/0/0 station is bit-for-bit unchanged in either knob state.
        // (Card UPO §4, requirement U1)
        {
            const Eigen::Vector3d hen = parser->getAntennaDeltaHen();
            const bool   has_record = parser->hasAntennaDeltaHen();
            const double ecc_norm   = hen.norm();
            const double abs_max    = hen.cwiseAbs().maxCoeff();
            if (!has_record) {
                // Left unchanged, with a warning (U1).  A missing record is not a zero
                // eccentricity: the coordinate cannot be reduced, so any eccentricity
                // the station has stays in the run as a modelling error, whether or
                // not the knob is armed.
                no_ecc_record_stations.push_back(cfg.name);
                log->warn("Network (card UPO): station '{}' has NO 'ANTENNA: DELTA H/E/N' record "
                          "in its RINEX header — its configured coordinate is used as given "
                          "(no marker -> ARP reduction possible). If that coordinate is a "
                          "monument coordinate the station is held below its antenna.",
                          cfg.name);
            } else if (ecc_norm > 1e-6) {
                ++n_ecc_stations;
                if (abs_max > 0.01) ++n_ecc_over_1cm;
                if (abs_max > ecc_max_abs) ecc_max_abs = abs_max;
                if (std::abs(hen(0)) > std::abs(ecc_max_h)) {
                    ecc_max_h = hen(0);
                    ecc_max_h_station = cfg.name;
                }
                if (marker_to_arp) {
                    st.ecef = Coordinates::markerToArp(st.ecef, hen);
                    ++n_ecc_reduced;
                }
                log->info("Network (card UPO): station '{}' ANTENNA: DELTA H/E/N "
                          "({:.4f}, {:.4f}, {:.4f}) m — marker -> ARP reduction {} "
                          "(network.station_marker_to_arp).",
                          cfg.name, hen(0), hen(1), hen(2),
                          marker_to_arp ? "APPLIED" : "NOT applied");
            }
        }
        st.is_pivot  = is_pivot;
        st.sm        = std::make_unique<StateManager>(5, log_manager_, config_);  // pos(3)+clk(1)+tropo(1)
        Eigen::VectorXd x0 = st.sm->getStateVector();
        x0.head<3>() = st.ecef;
        st.sm->setStateVector(x0);
        st.sm->setStateEpochTime(synced.front().time);
        st.model = std::make_unique<PppModel>(
            config_, *product_parser_, log_manager_, *parser, *st.sm);
        // Label the canonical-signal rider with the station name, unconditionally.  The
        // latch's own "locked for GPS …" line goes to the ppp_model logger, which is
        // empty in every run, so the labelled rider (RS-8, SPEC_event_harness §21.16)
        // is what shows a warm resume re-deciding a station's per-band channel.
        // The label is also the name matched against
        // processing.canonical_signal_priority_stations (card CSL-2): with that list
        // set it selects the station's canonical-signal rule; with the list empty it
        // changes log text only.
        st.model->setCanonicalTraceLabel(cfg.name);
        // Wind-up census (estimation.trace_windup_census): read-only per-receiver
        // step/gap counters, armed on every station so the run-end readout is a
        // roster sum.  The carry horizon is passed so the census can bucket the
        // gap population against it: the (wind-up horizon, carry horizon] bucket
        // is the invariant-#1 exposure that blocks arming accumulated mode on a
        // leveling lineage.
        if (config_.traceWindUpCensus())
            st.model->enableWindUpCensus(config_.phaseWindUpMaxGapS(),
                                         config_.networkPhaseLevelingMaxGapS());
        // Network coverage gate: restrict this station's model to the codes that
        // enough of the whole network tracks (empty allowlist ⇒ no-op, admit all).
        if (!signal_allowlist.empty()) st.model->setSignalAllowlist(signal_allowlist);
        // In brdc_rts the a-priori satellite clock is broadcast-grade, but the network
        // estimates the clock (Δδtˢ against that a-priori), so a producer station's
        // carrier phase is informative and must be kept.  The code-only
        // broadcast-phase gate is therefore suppressed on producer models only:
        // buildStation never builds the held-out closed-loop consumer (the roster loop
        // below skips it), so the consumer keeps the default gate.  sp3 mode passes
        // false (byte-identical).  (Card 5, RTF-1 / plan D1)
        st.model->setBroadcastPhaseGateSuppressed(brdc_rts);
        // Card NBP: a producer station's a priori satellite clock is the native broadcast
        // a priori (null in every other mode, byte-identical).  buildStation never builds
        // the held-out consumer, which reconstructs brdc(iode) + C from the store.
        st.model->setBroadcastClockApriori(setup.brdc_apriori.get());
        st.mm = std::make_unique<MeasurementModel>(
            *st.sm, *st.model, config_, log_manager_);
        // Phase-bias float gate: withhold satellite phase-bias states for signals
        // below network coverage (empty allowlist ⇒ no-op, every phase signal gets one).
        if (!phase_bias_allowlist.empty()) st.mm->setPhaseBiasSignalAllowlist(phase_bias_allowlist);
        // Seed this station's measurement-model frequency helper with its own
        // RINEX-header GLONASS FCN slots, so the GLONASS iono coefficient and
        // carrier wavelength in the Jacobian use the actual channel (not the
        // compiled-in default table).  Harmless for GPS/Galileo-only stations
        // (empty map).  Matters only under enableGlonassNetwork().
        st.mm->setGlonassSlots(parser->getGlonassFreqSlots());

        // Receiver-antenna ATX match check (startup diagnostic and the optional station
        // exclusion, network.exclude_unmodeled_antenna_stations).  A receiver antenna
        // type that matches no loaded ANTEX entry gets zero receiver PCO/PCV
        // (getAntennaCorrection returns only the satellite-side terms): a cm-level,
        // elevation-dependent phase-centre error that is otherwise visible only at trace
        // level (antenna.log is empty at default verbosity).  Checked once per station
        // here; the per-observation trace still fires.  Only meaningful when a receiver
        // correction would actually be applied.
        const auto& mc = config_.getModelCorrections();
        const bool rx_corr_on = (mc.enable_receiver_pco || mc.enable_receiver_pcv);
        if (rx_corr_on && !st.model->hasReceiverAntennaMatch()) {
            std::string rx_ant = parser->getReceiverAntennaType();
            const auto last = rx_ant.find_last_not_of(" \t\r\n");
            rx_ant = (last == std::string::npos) ? std::string{} : rx_ant.substr(0, last + 1);
            log->warn("Network: station '{}' receiver antenna '{}' has NO ATX model match "
                      "(even after the radome→NONE fallback) — receiver PCO/PCV NOT applied "
                      "(zero antenna correction on the receiver side).",
                      cfg.name, rx_ant.empty() ? "<none in RINEX header>" : rx_ant);

            // Tier 2 (network.exclude_unmodeled_antenna_stations): the always-on
            // radome→NONE fallback has already run, so a still-unmatched antenna is an
            // unknown type with entirely unmodelled receiver PCO/PCV, a systematic error
            // in every observation this station feeds into the shared clocks and biases.
            // With the knob armed the station is dropped from the network (a policy
            // backstop, off by default).
            if (config_.networkExcludeUnmodeledAntennaStations()) {
                // The clock-datum pivot realizes the datum and cannot be dropped; the
                // caller turns this outcome into a hard build error.
                if (is_pivot) return StationBuild::ExcludedUnmodeledAntenna;
                excluded_unmodeled_antenna_stations.push_back(cfg.name);
                log->warn("Network: station '{}' EXCLUDED from the network — unknown "
                          "receiver antenna TYPE (no ATX model; unmodelled receiver "
                          "PCO/PCV would be a systematic in the shared clocks/biases) "
                          "(network.exclude_unmodeled_antenna_stations).", cfg.name);
                return StationBuild::ExcludedUnmodeledAntenna;
            }

            // Kept (not excluded) but unmatched: recorded for the loaded-station summary
            // warning below.
            unmatched_antenna_stations.push_back(cfg.name);
        } else if (rx_corr_on) {
            // Matched.  A match that came only through the always-on type-preserving
            // radome→NONE fallback (mm-level radome-agnostic calibration) is logged once
            // per station, so the substitution is visible outside the per-observation
            // trace.
            std::string detail;
            if (st.model->receiverAntennaViaRadomeFallback(&detail)) {
                log->warn("Network: station '{}' receiver antenna {}", cfg.name, detail);
                radome_fallback_stations.push_back(cfg.name);
            }
        }

        // The receiver ATX frequency source per active constellation, on the two lowest
        // bands of its priority list (the dual pair: GPS 1 and 2, Galileo 1 and 5).  A
        // station whose route is not "exact" on some band gets its own line; every
        // station lands in the summary.  Measure-only.  (Card GAL, R6)
        if (rx_corr_on && atx_src_on) {
            std::string rx_ant = parser->getReceiverAntennaType();
            const auto last = rx_ant.find_last_not_of(" \t\r\n");
            rx_ant = (last == std::string::npos) ? std::string{} : rx_ant.substr(0, last + 1);
            std::string per_sys;
            bool any_not_exact = false;
            for (const std::string& sys_name : config_.getActiveSystems()) {
                const auto pit = config_.getSignalPriorities().find(sys_name);
                if (pit == config_.getSignalPriorities().end()) continue;
                const System sys = systemFromName(sys_name);
                if (sys == System::Unknown) continue;
                std::set<char> bands;   // the band digits of the priority list's code and phase entries
                for (const std::string& s : pit->second) {
                    const auto o = ObservableId::parse(sys, s);
                    if (o && (o->type == ObsType::Code || o->type == ObsType::Phase))
                        bands.insert(o->signal.digit);
                }
                std::string worst = "exact";
                std::string band_txt;
                int n_band = 0;
                for (const char b : bands) {
                    if (n_band++ >= 2) break;
                    std::string key;
                    // The band's phase with attribute C, "L<b>C".
                    const std::string sig = SignalId{sys, b, 'C'}.obsCode(ObsType::Phase);
                    const std::string route = st.model->receiverAtxFrequencySource(sig, sys, &key);
                    band_txt += fmt::format("{}band {} {}{}", band_txt.empty() ? "" : ", ", b,
                                            route, key.empty() ? std::string() : " (" + key + ")");
                    if (route == "none") worst = "none";
                    else if (route == "cross_system" && worst != "none") worst = "cross_system";
                    else if (route == "same_system" && worst == "exact") worst = "same_system";
                }
                if (worst != "exact") any_not_exact = true;
                atx_src_by_sys[sys_name][worst].push_back(cfg.name);
                per_sys += fmt::format("{}{}: {}", per_sys.empty() ? "" : " | ", sys_name, band_txt);
            }
            if (any_not_exact)
                log->warn("Network (card GAL, receiver ATX source): station '{}' antenna '{}' "
                          "does not resolve every band exactly: {}.  'cross_system' borrows "
                          "the nearest GPS band (model_corrections."
                          "enable_receiver_cross_system_atx_fallback); 'none' leaves the "
                          "receiver correction of that band at ZERO.",
                          cfg.name, rx_ant.empty() ? "<none in RINEX header>" : rx_ant, per_sys);
        }

        // Phase-leveling slip repair (T0, Increment 2): a persistent per-station MW
        // detector, with the MW + GF TurboEdit resolver under turbo_edit (see below),
        // for cycle-slip repair of the carried ambiguities.  On the legacy path it is
        // built for non-pivot stations only: the single pivot's ambiguities are all
        // gauge-pinned, and a repair there needs Increment 4 (design §6).  Under the
        // prioritized datum every station (including the former pivot) carries the
        // arcs it does not anchor and must slip-detect them, so it is built for all;
        // the PASS-A refloat still does nothing on a station's anchored (uncarried)
        // arcs.  network.cycle_slip.datum_station_detect extends this to the datum
        // station, which needs the same MW detector running for its slips to be
        // detected at all (§9.7.10, where the response is a network re-reference, not
        // a local refloat).  Constructed only when the carry and a detector are both
        // on, so the off path is byte-identical.  setCorrections(&this station's
        // GnssCorrections) engages the BIA-corrected wide-lane path (integer dN) where
        // biases are available; arc state lives on the detector and is station-local
        // (parallel-safe).
        const bool build_slip_for_pl =
            (config_.networkPhaseDatumPrioritized() || !is_pivot
             || config_.networkCycleSlipDatumStationDetect())
            && config_.networkPhaseLeveling()
            && (config_.networkCycleSlipTurboEdit() || config_.networkCycleSlipMwDetect());

        if (build_slip_for_pl) {
            st.mw_slip = std::make_unique<MwSlipDetector>(config_, log_manager_);
            // Every station's detector shares one logger, whose records are keyed
            // only by <sat>_<sig1>_<sig2>; the label is what lets the MWDEC / MWSKIP
            // decision lines be read per station.  Log text only; nothing else reads
            // the label.  Design: §14.10 of docs/SPEC_event_harness.md.
            st.mw_slip->setStationLabel(st.name);
            st.mw_slip->setCorrections(&st.model->getCorrections());
            // The TurboEdit MW+GF integer repair resolver is built only for turbo_edit
            // (higher-rate 1 s data, where the geometry-free ionospheric drift over the
            // epoch gap is small enough to trust an integer).  mw_detect uses the same MW
            // detector as a pure detector followed by a refloat, so it needs no resolver;
            // the null st.turbo_edit is what steers the PASS-A branch to the refloat path.
            if (config_.networkCycleSlipTurboEdit()) {
                st.turbo_edit = std::make_unique<TurboEditResolver>(config_, log_manager_);
                st.turbo_edit->setCorrections(&st.model->getCorrections());
            }
        }

        // IF-phase triple-difference slip detection: a persistent per-station
        // between-satellite/between-epoch ionosphere-free phase OMC detector.  On the
        // legacy path it is built for non-pivot stations only; under the prioritized
        // datum it is built for every station (the former pivot carries non-anchored
        // arcs that need detection).  Constructed whenever network.cycle_slip.if_phase_td
        // is on, independent of phase leveling, so it can run as a pure diagnostic in the
        // absolute solve; the refloat it drives is gated on the carry being active at the
        // call site.  Detector state is station-local (parallel-safe).
        if ((config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkCycleSlipIfPhaseTd()) {
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);
            st.if_slip->setDeferFlagging(true);  // network pools per-pair MAD; the driver flags
            // network.cycle_slip.if_phase_td_pairs = all evaluates every distinct band
            // pair, each on its own gradient chain, instead of only the first two
            // signals of each satellite.  The default `first` leaves the detector as it
            // was, so the off path is byte-identical; `all` closes the equal-count trap
            // on the pairs no chain watched.  Design: §14.10 of docs/SPEC_event_harness.md.
            st.if_slip->setAllPairs(
                config_.networkCycleSlipIfPhaseTdPairs() == NetworkIfPhaseTdPairs::All);
            // Push the hardened reference-satellite gate (Card R,
            // docs/SPEC_slip_qc_hardening.md §2); it is pushed only here, so the staged
            // user/kinematic path keeps the legacy gate (spec R5).  With
            // network.cycle_slip.if_phase_td_ref_gate off, rg.enable is false and the
            // gate is disabled (byte-identical).
            {
                const auto& sq = config_.getNetworkConfig().slip_qc;
                IfPhaseSlipDetector::RefGateParams rg;
                rg.enable          = sq.ref_gate;
                rg.k               = sq.ref_k;
                rg.min_n           = sq.ref_min_n;
                rg.thin_widen      = sq.ref_thin_widen;
                rg.coherence       = sq.ref_coherence;
                rg.max_consecutive = sq.ref_max_consecutive;
                rg.ewma_alpha      = sq.ref_ewma_alpha;
                rg.shrink_kappa    = sq.ref_shrink_kappa;
                rg.floor_frac      = sq.ref_floor_frac;
                rg.warmup_abstain  = sq.ref_warmup_abstain;   // item 1 of §6.14.6
                rg.elev_norm       = sq.ref_elev_norm;        // item 2 of §6.14.6
                rg.elev_clamp_deg  = sq.ref_elev_clamp_deg;
                st.if_slip->setRefGate(rg);
                // Run the reference arm on the additional chains too (REF-FAN,
                // §22.6); the loader disarms it unless pairs = all and ref_gate are
                // both set, and the detector refuses the combination a second time,
                // so pushing the raw flag here cannot reopen the noise-driven false
                // detections that §14.10.3 describes.
                st.if_slip->setRefArmAllChains(sq.ref_arm_all);
            }
        }

        st.pstate.position        = st.ecef;
        st.pstate.clock_bias      = 0.0;
        st.pstate.troposphere_zwd = 0.0;
        stations.push_back(std::move(st));
        return StationBuild::Built;
    };
    switch (buildStation(roster.reference, /*is_pivot=*/true)) {
        case StationBuild::Built:
            break;
        case StationBuild::ExcludedUnmodeledAntenna:
            log->error("Network: clock-datum station '{}' has an unknown receiver antenna "
                       "TYPE with no ATX model (even after the radome→NONE fallback) and "
                       "network.exclude_unmodeled_antenna_stations is set — the datum "
                       "station cannot be dropped. Supply its ATX model or choose a "
                       "different network.clock_datum_station. Aborting.",
                       roster.reference.name);
            return 1;
        case StationBuild::Failed:
            log->error("Network: clock datum station '{}' failed to load; the network "
                       "cannot be built without its datum. Aborting.",
                       roster.reference.name);
            return 1;
    }
    for (const auto& rv : roster.rovers) {
        if (closed_loop_on && rv.name == cl.station.name) continue;  // held-out consumer
        switch (buildStation(rv, /*is_pivot=*/false)) {
            case StationBuild::Built:
                break;
            case StationBuild::ExcludedUnmodeledAntenna:
                // Already warned + counted inside buildStation; the network proceeds
                // with the remaining stations.
                continue;
            case StationBuild::Failed:
                log->warn("Network: station '{}' failed to load; skipping it (the "
                          "network proceeds with the remaining stations).", rv.name);
                continue;
        }
    }
    log->info("Network: {} station estimators constructed (pivot '{}')",
              stations.size(), stations.front().name);
    if (!unmatched_antenna_stations.empty()) {
        std::string names;
        for (size_t i = 0; i < unmatched_antenna_stations.size(); ++i) {
            if (i) names += ", ";
            names += unmatched_antenna_stations[i];
        }
        log->warn("Network: {} of {} loaded station(s) have NO receiver antenna ATX match "
                  "(receiver PCO/PCV zeroed there): {} — supply their ATX models, or set "
                  "network.exclude_unmodeled_antenna_stations to exclude them",
                  unmatched_antenna_stations.size(), stations.size(), names);
    }
    // Join a name vector into a comma-separated list for the summaries below.
    auto joinNames = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s;
    };
    // Receiver ATX source summary per constellation: the route of each station's
    // worst band (exact < same_system < cross_system < none).  (Card GAL, R6)
    for (const auto& [sys_name, by_route] : atx_src_by_sys) {
        std::string t;
        for (const char* route : { "exact", "same_system", "cross_system", "none" }) {
            const auto it = by_route.find(route);
            const std::size_t n = (it == by_route.end()) ? 0 : it->second.size();
            t += fmt::format("{}{} {}", t.empty() ? "" : "; ", route, n);
            if (n > 0 && std::string(route) != "exact")
                t += " [" + joinNames(it->second) + "]";
        }
        log->info("Network (card GAL, receiver ATX source, {}): per station, the route of "
                  "its worst band on the dual pair: {}.  (cross-system fallback {}.)",
                  sys_name, t,
                  config_.getModelCorrections().enable_receiver_cross_system_atx_fallback
                      ? "ARMED" : "off");
    }
    // Tier 1 (always-on radome→NONE): report the stations whose receiver PCO/PCV
    // came from the radome-agnostic NONE calibration (mm-level).  They are matched,
    // not dropped; one summary line keeps the substitution visible.
    if (!radome_fallback_stations.empty()) {
        log->info("Network: {} of {} loaded station(s) matched their receiver antenna via "
                  "the type-preserving radome→NONE fallback (radome uncalibrated; NONE "
                  "PCO/PCV applied, mm-level): {}",
                  radome_fallback_stations.size(), stations.size(),
                  joinNames(radome_fallback_stations));
    }
    // Tier 2 (network.exclude_unmodeled_antenna_stations): the count of stations
    // dropped because their receiver antenna type is unknown to the ANTEX even after
    // the radome→NONE fallback, logged at setup as the knob's on-path telemetry
    // (invariant #4).  With the knob armed and nothing dropped, the zero is logged.
    if (!excluded_unmodeled_antenna_stations.empty()) {
        log->warn("Network: {} station(s) excluded (unmodeled receiver antenna): {}",
                  excluded_unmodeled_antenna_stations.size(),
                  joinNames(excluded_unmodeled_antenna_stations));
    } else if (config_.networkExcludeUnmodeledAntennaStations()) {
        log->info("Network: 0 stations excluded (unmodeled receiver antenna) — every "
                  "loaded station's receiver antenna resolved (radome→NONE fallback "
                  "covered any uncalibrated radomes).");
    }

    // --- Marker → ARP summary (card UPO §4, requirement U2) -----------------
    // Emitted in both knob states.  With the knob on it records the roster's frame;
    // with it off it measures the error the run carries, which is the number the
    // fix is judged against (standing invariant #4).
    setup.n_marker_arp_ecc       = n_ecc_stations;
    setup.n_marker_arp_reduced   = n_ecc_reduced;
    setup.n_marker_arp_over_1cm  = n_ecc_over_1cm;
    setup.n_marker_arp_no_record = static_cast<int>(no_ecc_record_stations.size());
    setup.marker_arp_max_h       = ecc_max_h;
    setup.marker_arp_max_h_station = ecc_max_h_station;
    if (marker_to_arp) {
        log->info("Network (card UPO): marker -> ARP roster reduction ON — {} of {} loaded "
                  "station(s) carry an 'ANTENNA: DELTA H/E/N' eccentricity ({} above 0.01 m), "
                  "{} reduced to the antenna reference point; largest H {:+.4f} m at '{}'. "
                  "The roster is now in the SAME frame as the antenna model and as the "
                  "held-out consumer's truth.",
                  n_ecc_stations, stations.size(), n_ecc_over_1cm, n_ecc_reduced,
                  ecc_max_h, ecc_max_h_station.empty() ? "<none>" : ecc_max_h_station);
    } else if (n_ecc_over_1cm > 0) {
        log->warn("Network (card UPO): marker -> ARP roster reduction OFF — {} of {} loaded "
                  "station(s) carry an eccentricity above 0.01 m (largest component {:.4f} m; "
                  "largest H {:+.4f} m at '{}') and are held at their MONUMENT coordinate while "
                  "their observations refer to the antenna. Each such station misfits every "
                  "range by about H*sin(elev); its zenith wet delay absorbs roughly a quarter "
                  "of H and the remainder reaches the satellite clocks regionally "
                  "(docs/refound/UPO_consumer_up_offset_2026-09-22.md). Set "
                  "network.station_marker_to_arp to reduce them.",
                  n_ecc_over_1cm, stations.size(), ecc_max_abs, ecc_max_h,
                  ecc_max_h_station.empty() ? "<none>" : ecc_max_h_station);
    } else {
        log->info("Network (card UPO): marker -> ARP roster reduction OFF — no loaded station "
                  "carries an eccentricity above 0.01 m ({} with any nonzero record), so the "
                  "roster's monument and antenna-reference-point frames coincide to within a "
                  "centimetre.", n_ecc_stations);
    }
    if (!no_ecc_record_stations.empty())
        log->warn("Network (card UPO): {} of {} loaded station(s) carry NO 'ANTENNA: DELTA "
                  "H/E/N' record and cannot be reduced in either knob state: {}",
                  no_ecc_record_stations.size(), stations.size(),
                  joinNames(no_ecc_record_stations));

    // --- Station roster (viz) -----------------------------------------------
    // A tiny static CSV of the stations that actually loaded (id, role, ECEF) so
    // the pyviz worldmap reflects the real run, not just the config.  The
    // held-out closed-loop consumer (if any) is listed with role "held_out".
    if (nc.trace_stations)
        NetworkRunTraces::writeStationRoster(log_dir_, stations,
                                             closed_loop_on ? &cl.station : nullptr, log);

    // === Per-station compute parallelism ====================================
    // PASS A (geometry) and the produce half of PASS B (measurement assembly) are
    // embarrassingly parallel across stations: each owns an independent
    // StateManager / PppModel / MeasurementModel, and the audited call trees touch
    // no shared mutable state (product stores are const after load; the
    // per-instance model caches are thread-private; the live SSR/ephemeris
    // singletons are mutex-guarded, and in post-processing they are empty except
    // for the LiveEphemerisStore that brdc_rts fills from the serial part of the
    // epoch loop).  The shared-estimator scatter (est.addStation) and everything
    // downstream of it stay serial, in deterministic station order, so the float
    // solution is bit-identical to a serial run for any thread count.  Parallelism
    // is used only above the configured station-count threshold
    // (network.parallel_min_stations): small networks run serially, with no
    // fork-join cost, and the internal 3-station fixture is unchanged.
    int hw_max_threads = 1;
#ifdef _OPENMP
    hw_max_threads = omp_get_max_threads();
#endif
    setup.n_sta_total = static_cast<int>(stations.size());
    const int n_sta_total = setup.n_sta_total;
    setup.parallel_on =
        nc.parallel_stations
        && n_sta_total > nc.parallel_min_stations
        && hw_max_threads > 1;
    const bool parallel_on = setup.parallel_on;
    setup.n_threads = parallel_on
        ? (nc.parallel_max_threads > 0
               ? std::min(nc.parallel_max_threads, hw_max_threads)
               : hw_max_threads)
        : 1;
    const int n_threads = setup.n_threads;
    // Deterministic-parallel post-solve seam (network.parallel_seam): the per-block
    // DD carry fold and the D5 innovation-gate readouts.  It rides on the per-station
    // parallel path (same thread budget, Eigen already pinned to one thread for the
    // epoch) and is byte-identical at any thread count: blocks are computed in parallel and accumulated/installed serially in
    // fixed block order.  With seam_threads == 1 the seam runs its original serial
    // path, so with the knob off the run is byte-for-byte the run without the seam.
    const bool seam_on      = parallel_on && nc.parallel_seam;
    setup.seam_threads = seam_on ? n_threads : 1;
    const int  seam_threads = setup.seam_threads;
    if (seam_on)
        log->info("Network: post-solve carry-fold/D5 seam PARALLEL across {} threads "
                  "(deterministic — byte-identical to the serial fold)", n_threads);
    // network.parallel_carry: thread the Card-4 carry's dense per-block algebra
    // (buildDdPriors W·Pᶻ·Wᵀ, foldReducedDdPriors, installPosterior) through Eigen's
    // own multi-threaded GEMM.  The seam parallelizes the carry only over its few
    // blocks; this un-pins Eigen so each block's O(K²–K³) matmul spreads across
    // cores.  Those stages then run serially over blocks (n_threads = 1), so Eigen
    // threading does not nest inside the seam's OpenMP.  The dense products are
    // reassociated, which drifts from the serial result at the ~1e-X ULP level; the
    // default (off) keeps Eigen pinned and the seam/serial path byte-identical.
    setup.carry_par = parallel_on && nc.parallel_carry;
    const bool carry_par = setup.carry_par;
    if (carry_par)
        log->info("Network: Card-4 carry dense algebra Eigen-THREADED across {} threads "
                  "(buildDdPriors / foldReducedDdPriors / installPosterior; serial≡parallel "
                  "byte-identity intentionally relaxed to ~1e-X — physically null)", n_threads);
#ifndef _OPENMP
    if (nc.parallel_stations && n_sta_total > nc.parallel_min_stations)
        log->warn("Network: parallel_stations requested but this build has no OpenMP "
                  "support; per-station compute will run serially.");
#endif
    if (parallel_on)
        log->info("Network: per-station compute PARALLEL — {} stations across {} threads "
                  "(shared-estimator scatter stays serial; solution is thread-count-invariant)",
                  n_sta_total, n_threads);
    else
        log->info("Network: per-station compute SERIAL ({} stations)", n_sta_total);

    // Prewarm every logger the per-station model path can touch.  getLogger()'s
    // first-touch create path mutates a shared std::map; fetching an existing logger
    // is lock-free and data-race-safe.  Creating each one now, single-threaded,
    // ensures that no insert races a concurrent fetch inside the parallel region,
    // the only shared-mutable hazard the thread-safety audit found in the
    // per-station call tree.
    //
    // The slip detectors' loggers ("mw_slip_detector", "if_slip_detector") are not
    // on this list: the detector constructors create them in this serial setup
    // phase above, before any parallel region runs, so the create race cannot
    // occur.  If detector construction moves out of the serial setup, add them
    // here.  (INC-7 re-audit)
    if (parallel_on) {
        for (const char* lname : {
                "ppp_model", "geometry", "antenna", "antenna_model", "corrections",
                "planetary_model", "ocean_tide", "ppp_utils", "ionosphere",
                "troposphere", "stochastic_model", "measurement_model",
                "state_manager", "state_initializer", "bssd_transform",
                "ambiguity_resolver", "main_log" })
            (void)log_manager_.getLogger(lname);
    }

    // --- Owned product sink (closed-loop isolation) -------------------------
    SsrCorrectionStore& network_store = setup.network_store;
    // The clock-disc toggle makes the emitter maintain and stamp the per-satellite clock
    // discontinuity counter (A2).  The phase-bias disc threshold is the emitter's own
    // default constant, the single source of truth shared with the serialized contract's
    // disc_threshold_m (R9).  The staleness horizon (product-time seconds; 0 = off) turns
    // a key that returns after a longer gap into a first sighting, so no disc counter is
    // bumped against an hours-stale baseline (RTA-11).  Every construction parameter also
    // travels in the serialized contract (pending_contract below).
    setup.emitter.emplace(NetworkProductEmitter::kDefaultDiscThresholdM,
                          config_.networkEmitClockDisc(),
                          config_.networkEmitterStalenessHorizonS(),
                          config_.networkCodeBiasDiscJumpTest(),
                          config_.networkClockDiscOnSetChange(),
                          config_.networkDiscThresholdFracCycles(),
                          nc.disc_mark_scope == "channel");  // per-channel mark scope (Card MS)
    NetworkProductEmitter& emitter = *setup.emitter;

    // --- Optional on-disk product serialization -----------------------------
    // A reusable per-epoch dump of the emitted solution (the same pre-emit
    // NetworkSolutionEpoch the in-memory closed loop consumes, plus this epoch's
    // datum-change flag).  Consumer-side experiments, such as the consumer AR-frame
    // work, can replay the products without re-running the multi-station network:
    // NetworkProductReader::replay reproduces this store byte-for-byte
    // (discontinuity counters included).
    auto& product_writer = setup.product_writer;
    auto& prod_path = setup.prod_path;
    // The writer is opened at the first emit, not here, so that the realized
    // per-constellation clock pivots (auto-picked, unknown until the first solve) are
    // stamped into the v2 header, not only the config intent.  pending_contract holds
    // everything known up front; the realized pivots merge in at the first emit, and
    // contract_pending gates that one-shot construction.  (B6, P1-1)
    auto& pending_contract = setup.pending_contract;
    auto& contract_pending = setup.contract_pending;
    if (!nc.product_output_path.empty()) {
        // A relative path lands under the network trace dir, alongside
        // network_solution.csv etc.; an absolute path is used verbatim.
        prod_path = nc.product_output_path;
        if (prod_path.is_relative()) prod_path = log_dir_ / prod_path;
        // The S-basis contract is part of the wire and is always stamped.  A replay of a
        // wire without it rebuilds the default emitter (no clock-discontinuity channel,
        // the flat default phase-bias threshold) and misses every declared clock
        // re-reference, so the declaration cannot depend on an output option.  The
        // records are the same byte for byte as a contract-less writer's; only the
        // header gains the S line.  (Card OWD, O1)
        {
            // Stamp the S-basis contract into a v2 header (B6).  The declaration semantics
            // are the A3-corrected per-constellation satellite-clock-pin basis.
            pending_contract.decl.reference_receiver_id = nc.clock_datum_station;
            pending_contract.decl.emit_code_bias        = config_.estimateNetworkSatCodeBias();
            // Self-describe the phase-bias gauge in the on-disk contract (T3b).  Under
            // phase_datum_eq34 the emitted bias references a single-receiver chain
            // (Card 3), not a pivot receiver.  The per-solve decl in NetworkSolveGlue.cpp
            // makes the same choice, but that object feeds only buildSharedDatum: the
            // serialized declaration is this one.  Without this branch the product would declare
            // amb_datum = 0 (PivotReceiverZeroAmbiguity), the legacy single-pivot datum it
            // does not use, and an eq34 wire would contradict itself by also carrying pdr=
            // (stamped only under eq34) two fields later.  A consumer trusting field 5 would
            // build its between-satellite reference on the wrong frame; the enumerator
            // exists so that such a consumer can refuse on "amb_datum differs".  Older eq34
            // wires, written before the eq34 branch, carry that contradiction.  The branch
            // mirrors the isb_datum self-description that follows; with the knob off the
            // default stays (byte-identical).
            if (config_.networkPhaseDatumEq34())
                pending_contract.decl.amb_datum =
                    SbasisDeclaration::AmbDatum::SingleReceiverChain;
            if (config_.networkClockDatumBroadcastAnchored()) {
                // Broadcast-anchored clock datum (brdc_rts): declare the per-satellite
                // broadcast-anchor basis and leave clock_pivot_sat empty (no pivot; a
                // config pin is ignored, warned at load).  The anchor sigma is part of
                // the realized gauge, so record it in the declaration.
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
                pending_contract.decl.broadcast_anchor_sigma_m =
                    config_.resolveNetworkAprioriClockAnchorSigmaM();
            } else if (config_.networkClockDatumSumToZero()) {
                // Anchor-free clock datum: declare the sum-to-zero basis and leave
                // clock_pivot_sat empty (there is no pivot; a config pin is ignored).
                // The getter is read directly: the run-loop copy
                // (ctx.knobs.clock_sum_to_zero) is built after setup.
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
            } else if (!nc.clock_datum_satellite.empty()) {
                pending_contract.decl.clock_pivot_sat[
                    constellationKey(systemOfSat(nc.clock_datum_satellite))]
                    = nc.clock_datum_satellite;
            }
            // Resolve the a-priori orbit/clock identity to the resolved SP3 and CLK file
            // basenames (the baseline the emitted clock C0 is a delta against); the config
            // intent alone is "CODFIN00000000" under auto-detect.  (P1-1)
            const auto& dl = config_.getDownloadConfig();
            std::string apriori_id =
                fmt::format("{}{}{:04d}{:03d}", dl.product_source, dl.product_type, dl.year, dl.doy);
            if (config_.networkAprioriBrdcClock()) {
                // Card NBP: the SP3 files in force, the navigation file and the rule
                // ("sp3+brdc:<SP3>|<nav>|ttom").  A consumer (and the replay driver) reads
                // this prefix as "the clock refers to the stamped broadcast ephemeris" and
                // refuses a CLK-bearing root under it.
                bool named_all = false;
                std::string sp3 = aprioriIdentityForWindow(
                    config_.getProductPaths(), setup.window_t0_s, setup.window_t1_s, &named_all);
                // The helper names "<SP3>+<CLK>" with "?" for a kind that has no file; the
                // mode loads no CLK, so the CLK half ("+?") is dropped.
                if (sp3.size() >= 2 && sp3.compare(sp3.size() - 2, 2, "+?") == 0)
                    sp3.resize(sp3.size() - 2);
                const std::string nav = config_.getNavigationPath().filename().string();
                apriori_id = std::string(kNbpAprioriPrefix) + (sp3.empty() ? std::string("?") : sp3)
                           + "|" + (nav.empty() ? std::string("?") : nav) + "|ttom";
            } else if (brdc_rts) {
                // The a-priori is broadcast ephemeris, so the identity is the nav-file
                // basename ("brdc:<file>").  Once an RTS-orbit source is attached (plan D9
                // seam / RTF-3), this becomes "brdc+rts:<mount>".  (Card 5, plan D10)
                const std::string nav = config_.getNavigationPath().filename().string();
                apriori_id = "brdc:" + (nav.empty() ? std::string("?") : nav);
            } else {
                // Name the files in force during the processing window.  The resolver merges
                // the previous, the observation and the next day's products, and the first
                // merged SP3 / CLK (getProductPath) is the previous day's, so naming it would
                // declare the previous day's products (DOY-128 on a DOY-129 wire).
                // (Card OWD, O1)
                bool named_all = false;
                const std::string in_force = aprioriIdentityForWindow(
                    config_.getProductPaths(), setup.window_t0_s, setup.window_t1_s, &named_all);
                if (!in_force.empty()) apriori_id = in_force;
                if (named_all)
                    log->info("Network (card OWD): a product file name does not carry an IGS "
                              "long-name span (or none meets the processing window), so the S "
                              "contract's a-priori identity names every merged file of that "
                              "kind: '{}'.", apriori_id);
            }
            pending_contract.apriori_product_id = apriori_id;
            // Contract identity fields, each serialized as an optional key=value S-line
            // field only when non-empty (RTA-12):
            //  - frame_id / coord_epoch are config declarations (network.frame_id /
            //    network.coord_epoch; the SIF-coordinate path defaults coord_epoch to
            //    the propagation date), with no auto-detection.
            //  - atx_id is the resolved antenna-calibration file this run loaded (the
            //    basename of the merged product path).  When no .atx resolved,
            //    getProductPath returns an empty path and the atx= field is absent; a
            //    consumer that pins expect_atx_id then flags "file '' vs expected ...".
            pending_contract.frame_id    = nc.frame_id;
            pending_contract.coord_epoch = nc.coord_epoch;
            pending_contract.atx_id      = config_.getProductPath("ATX").filename().string();
            //  - orbit_ref: in sp3 mode, SP3 orbits and ATX satellite PCO/PCV mean the
            //    emitted clock C0 is referred to the satellite centre of mass.  The value
            //    is hardcoded, in every a-priori mode, until an SSR orbit source
            //    (APC-referred, per the RTCM SSR convention) lands; then it becomes a
            //    variable of the orbit path selection.
            pending_contract.orbit_ref   = "CoM";
            pending_contract.disc_threshold_m   = NetworkProductEmitter::kDefaultDiscThresholdM;  // same constant as the emitter above (R9)
            // The emitter's construction parameters (the disc threshold above, the
            // clock-disc channel flag, the RTA-11 staleness horizon, and the jump-test and
            // mark-scope options below) travel in the contract, so a consumer can replay
            // the file with byte-identical discontinuity counters and no out-of-band
            // knowledge of the producer's config.  The horizon serializes as an optional
            // stale= field only when non-zero, so a run with it off writes a
            // byte-identical v2 header.
            pending_contract.emit_clock_disc    = config_.networkEmitClockDisc();
            pending_contract.staleness_horizon_s = config_.networkEmitterStalenessHorizonS();
            pending_contract.code_bias_jump_test = config_.networkCodeBiasDiscJumpTest();
            pending_contract.clock_disc_on_set_change = config_.networkClockDiscOnSetChange();
            pending_contract.disc_threshold_frac_cycles = config_.networkDiscThresholdFracCycles();  // per-λ jump gate, discfrac= (H3)
            pending_contract.disc_mark_scope_channel = (nc.disc_mark_scope == "channel");  // per-channel mark scope, discscope= (Card MS)
            pending_contract.emit_pb_hold = nc.emit_pb_hold;  // the phase-bias hold reading rule, pbhold= (Card PBH)
            pending_contract.emit_pb_hold_phase_clock =       // pbhold=2: the phase-visible clock is held (Card LAH)
                nc.emit_pb_hold && nc.emit_pb_hold_phase_clock;
            pending_contract.emit_pb_hold_smoothed =          // pbhold=3: the smoothed hold (Card SMH)
                nc.emit_pb_hold && nc.emit_pb_hold_smoothed;
            // Advertise the combined-σ age-scaling coefficient on the S line only when the
            // combined-σ channel is on and a coefficient is set, so a run with the channel
            // off (or agek = 0) writes a byte-identical v2 header (no agek= token).
            // (M4, contract v2)
            if (config_.networkEmitCombinedSigma())
                pending_contract.age_scale_k = config_.networkCombinedSigmaAgek();
            // Advertise which statistic the H heartbeat's sigma0 carries, only when the
            // census feed is armed (otherwise no hsig= token, byte-identical).  The census
            // σ̂₀ is a different population from the within-epoch RAIM's, and a consumer
            // thresholding field 5 needs to know which one it reads.  (H-rider)
            if (config_.networkHSigma0PrefitCensus())
                pending_contract.h_sigma0_source = "pc";
            pending_contract.present            = true;
            contract_pending = true;
            // The deferred writer opens (and truncates) only at the first emit, so a run
            // that emits no epochs would leave a previous run's complete product file at
            // this path for a downstream consumer to replay unnoticed.  Remove any
            // existing file now, as a truncate at startup would.
            std::error_code stale_ec;
            std::filesystem::remove(prod_path, stale_ec);
            if (stale_ec && std::filesystem::exists(prod_path))
                log->warn("Network: could not remove a stale product file at '{}' ({}) — "
                          "if this run emits no epochs, a downstream consumer would replay "
                          "the PREVIOUS run's products from that path.",
                          prod_path.string(), stale_ec.message());
            log->info("Network: will serialize per-epoch products with a v2 S-basis contract "
                      "(realized pivots stamped at first emit) to '{}'", prod_path.string());
        }
    }

    // --- Closed-loop consumer (held-out station) ----------------------------
    // The held-out station runs the user-side PPP-RTK and AR pipeline through a
    // RealTimeEngine fed epoch by epoch from RINEX replay, with its SSR consumption
    // pointed at the owned network_store, never the singleton (with
    // closed_loop.consume_network_products off it runs on precise products only, as
    // a baseline).  Each epoch the producer emits into network_store and the consumer
    // then reads that epoch, which tests whether the project's own user engine
    // recovers integers from the producer's products on an independent station.
    setup.consumer_config = config_.deriveClosedLoopConsumerConfig();
    GnssConfig& consumer_config = setup.consumer_config;
    auto& consumer = setup.consumer;
    setup.cl_truth_arp = Eigen::Vector3d(cl.station.position_ecef.data());
    if (closed_loop_on) {
        // Reduce the configured (monument-referenced) truth into the ARP frame the
        // consumer estimates in, using the held-out station's own RINEX header.  A
        // zero eccentricity leaves the value bit-identical, so the current fixture
        // stations (CIT1, NRMG: 0/0/0) grade exactly as without the reduction.
        if (const RinexParser* cl_parser = sync.parserOf(cl.station.name)) {
            setup.cl_truth_hen = cl_parser->getAntennaDeltaHen();
            setup.cl_truth_arp = Coordinates::markerToArp(setup.cl_truth_arp, setup.cl_truth_hen);
            if (setup.cl_truth_hen.norm() > 1e-6)
                log->info("Network closed-loop: truth for '{}' reduced marker -> ARP via "
                          "ANTENNA: DELTA H/E/N ({:.4f}, {:.4f}, {:.4f}) m — the consumer state "
                          "is antenna-referenced, the configured coordinate is the monument; "
                          "grading them directly would bias up_m by the eccentricity.",
                          cl.station.name, setup.cl_truth_hen(0),
                          setup.cl_truth_hen(1), setup.cl_truth_hen(2));
        } else {
            log->warn("Network closed-loop: no parser for held-out station '{}' — truth used "
                      "as configured (no marker -> ARP eccentricity reduction).", cl.station.name);
        }
        // Point the consumer's RinexParser at the held-out station so its header
        // (antenna type, approximate position) seeds the consumer correctly; the
        // per-epoch observations themselves come from the synced roster in the epoch
        // loop.
        consumer_config.setObservationPath(cl.station.rinex_path);
        consumer = std::make_unique<RealTimeEngine>(consumer_config, log_manager_);
        if (!consumer->initialize()) {
            log->error("Network closed-loop: consumer engine initialize() failed; "
                       "disabling the harness for this run.");
            consumer.reset();
        } else {
            // Direct LAMBDA on the network-bias-corrected float must not also take the
            // MW WL, which is in the .BIA frame (see setArIgnoreMwWl).  wl_only AR keeps
            // the MW WL by definition.
            consumer->setArIgnoreMwWl(!consumer_config.getArWlOnly());
            // Arm the per-satellite readout (network.trace_consumer_sat) before the
            // product-routing branch below, so the instrument also exists on the
            // baseline arm (consume_network_products = false): a consumer that consumes
            // no products still has ambiguities, iono states and residuals, and the A/B
            // between the two arms is one of the comparisons this trace serves.
            // (Card CST)
            consumer->setConsumerSatTrace(nc.trace_consumer_sat);
            const bool undiff = cl.consumer_undifferenced;
            if (undiff)
                log->info("Network closed-loop: UNDIFFERENCED user model — direct per-band "
                          "LAMBDA on the network-bias float (no BSSD, no WL/NL cascade); the "
                          "resolver forms the between-satellite difference internally. AR fires "
                          "once the 3D position σ converges below the FORCE-CAPPED gate "
                          "({:.2f} m; undiff NL LAMBDA is clamped to ≤0.20 m at load — "
                          "GnssConfigLoad applyNetworkConsumerCouplings). T2c: the per-epoch "
                          "'ar_gate' column of closed_loop_trace.csv attributes which gate "
                          "(sigma_gate/min_epochs/no_candidates/ratio_battery) zeroed AR.",
                          consumer_config.getArMaxPositionSigma());
            else if (!consumer_config.getArWlOnly())
                log->warn("Network closed-loop: BSSD consumer with ar_wl_only=false runs PURE "
                          "full NL LAMBDA on the float — EXPERIMENTAL and slow on the v1 product "
                          "(the WL-float-level position makes the NL search ill-conditioned).  "
                          "Prefer ar_wl_only=true, or consumer_undifferenced=true.");
            if (cl.consume_network_products) {
                // Consume the producer's own products (never the singleton).
                // consume_network_clock gates the clock overlay: false keeps precise
                // SP3/CLK and consumes only the network phase bias (the clean
                // integer-recovery test).
                consumer->setSsrStore(&network_store, cl.consume_network_clock, /*bias=*/true);
                // Strong (ionosphere-corrected) user model: consume the network's SH-VTEC
                // slant iono as a tight per-satellite BLUP prior (the producer fills the
                // store's slant-iono channel each epoch in the run loop).  Off by default:
                // the consumer floats its own iono (byte-identical).
                consumer->setConsumeNetworkIono(cl.consume_network_iono);
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — reading the OWNED network store ({}{}).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off",
                          cl.consume_network_clock
                              ? "full: network clock + phase bias"
                              : "bias-only: precise clock + network phase bias",
                          cl.consume_network_iono ? " + SH-VTEC slant iono" : "");
            } else {
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — BASELINE mode (precise products only, network "
                          "store NOT injected).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off");
            }
        }
    }

    return 0;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the marker to ARP roster reduction (UPO).
int networkMarkerArpRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // Marker to antenna-reference-point roster frame (card UPO), reported in both knob
    // states (invariant #4).  With the knob off this line gives the size of the modelling
    // error the run carried, the number the fix is judged against; armed, it states that
    // the roster is held at the antenna reference points rather than the published
    // monument coordinates, which a reader of the products has to know.
    if (!(ctx.counters.n_marker_arp_ecc > 0 || ctx.counters.n_marker_arp_no_record > 0)) return 0;
    if (config_.networkStationMarkerToArp())
        log->info("Network (card UPO): marker -> ARP roster reduction ON — {} station(s) "
                  "carried an eccentricity ({} above 0.01 m) and {} were reduced to their "
                  "antenna reference point; largest H {:+.4f} m at '{}'; {} station(s) had "
                  "no header record and were left at their configured coordinate.",
                  ctx.counters.n_marker_arp_ecc, ctx.counters.n_marker_arp_over_1cm,
                  ctx.counters.n_marker_arp_reduced, ctx.counters.marker_arp_max_h,
                  ctx.counters.marker_arp_max_h_station.empty()
                      ? "<none>" : ctx.counters.marker_arp_max_h_station,
                  ctx.counters.n_marker_arp_no_record);
    else
        log->info("Network (card UPO): marker -> ARP roster reduction OFF — {} station(s) "
                  "carried an eccentricity ({} above 0.01 m; largest H {:+.4f} m at '{}') "
                  "and were held at their MONUMENT coordinate for the whole run, while "
                  "their observations refer to the antenna; {} station(s) had no header "
                  "record. This count is the size of the class on this run "
                  "(network.station_marker_to_arp).",
                  ctx.counters.n_marker_arp_ecc, ctx.counters.n_marker_arp_over_1cm,
                  ctx.counters.marker_arp_max_h,
                  ctx.counters.marker_arp_max_h_station.empty()
                      ? "<none>" : ctx.counters.marker_arp_max_h_station,
                  ctx.counters.n_marker_arp_no_record);
    return 0;
}

// ---- End of the run-end reports ----

// Setup builder (card NOM3 step EP4); documented at its declaration in app/NetworkSetupResult.hpp.
void networkMarkerArpCensusCopy(NetworkRunContext& ctx, const GnssConfig& config_, const NetworkSetupResult& setup) {
    // ---- End of the bindings ----
    // The marker → ARP roster census is measured in networkSetup_, which runs
    // before this context exists.  It is copied into the counters because the
    // run-end readout and the run manifest read ctx.counters, and both report it
    // in either knob state (card UPO).
    ctx.counters.marker_arp_armed         = config_.networkStationMarkerToArp();
    ctx.counters.n_marker_arp_ecc         = setup.n_marker_arp_ecc;
    ctx.counters.n_marker_arp_reduced     = setup.n_marker_arp_reduced;
    ctx.counters.n_marker_arp_over_1cm    = setup.n_marker_arp_over_1cm;
    ctx.counters.n_marker_arp_no_record   = setup.n_marker_arp_no_record;
    ctx.counters.marker_arp_max_h         = setup.marker_arp_max_h;
    ctx.counters.marker_arp_max_h_station = setup.marker_arp_max_h_station;
}

} // namespace GnssProcessor
