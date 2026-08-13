/**
 * @file ApplicationNetworkSetup.cpp
 * @brief @internal Application::runNetwork setup phases (decomposition INC-5,
 *        2026-07-16) — Phase-0 input validation and Phases 2-4 + product sink
 *        + S-basis contract + closed-loop consumer, relocated VERBATIM from
 *        ApplicationNetwork.cpp.  Outputs land in NetworkSetupResult (filled
 *        in place; see its header for the address-stability contract).  Every
 *        former early `return 1` is preserved with its identical log line.
 *        Include block mirrors the source TU (INC-1 precedent); the cleanup
 *        pass is deferred to INC-12.
 */

/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * Extracted from Application.cpp to keep the per-mode processing pipelines in
 * dedicated translation units (mirrors ApplicationBaseline.cpp /
 * ApplicationRealtime.cpp).  The network producer is a joint multi-station
 * estimator: per-station local normals reduced onto a shared satellite-
 * parameter block via an exact Schur complement, estimating the estimable
 * satellite clock + satellite phase bias under an explicitly declared
 * minimum-constraint S-basis, emitted in IGS-SSR convention for the companion
 * PPP_RTK_User consumer.
 *
 * See docs/ssystem_conformance_2026-06-15.md (§7) and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * ===========================================================================
 * W4-S2b — the live epoch driver
 * ===========================================================================
 *
 * Per solution epoch the driver runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  for each present station: PppModel::computeModels →
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-sat clock + per-(sat,signal) phase bias) using the
 *       SAME usability gate the measurement build uses.  The DATUM (pivot)
 *       station is processed first so its usable phase-signal set restricts the
 *       phase-bias columns to ones backed by a pivot-receiver ambiguity pin.
 *       The estimator's shared column layout is then registered (clock for every
 *       satellite any station sees; phase bias for the pivot's signal set).
 *
 *   PASS B (assembly)  for each present station: MeasurementModel::compute-
 *       MeasurementUpdate produces an undifferenced H/innovations/R block (no
 *       receiver-side differencing — DifferencingMode::None).  Its columns are
 *       split: the satellite clock (−1) and phase-bias (+1) columns scatter into
 *       the shared border A_s; the 3 receiver-position columns are DROPPED
 *       (coordinates held fixed); the rest become the station's private nuisance
 *       block A_r.  The pivot station additionally folds tight-prior pins on its
 *       own ambiguities (the LOCAL half of the S-basis — the emitted phase bias
 *       then carries that pivot-receiver ambiguity lump, Odijk Eq. 4).
 *
 *   SOLVE  NetworkSbasis builds the SHARED minimum-constraint datum (one
 *       satellite-clock pin per constellation), the null-space rank gate
 *       verifies the reduced shared system carries exactly that deficiency, then
 *       NetworkEstimator::solve produces the shared estimate.  NetworkProduct-
 *       Emitter serializes it into an OWNED SsrCorrectionStore (closed-loop
 *       isolation — never the process-wide singleton the user side reads).
 *
 *   D2 (between-epoch)  the shared posterior (value, variance) is carried
 *       forward as a diagonal prior on the next epoch's shared border, inflated
 *       by the per-parameter process noise over the epoch gap.
 *
 * Scope (V1): estimable satellite CLOCK + PHASE BIAS, coordinates/orbits fixed,
 * GPS + Galileo, per-band uncombined, float (no network AR).  Satellite code
 * bias, atmosphere products, real-time branch, GLONASS FCN bias, and network QC
 * are later slices (W6/W7/Phase 2).  Each station's coordinates MUST be known
 * (held fixed) and its observation RINEX path MUST be explicit in the config —
 * the PathResolver network gate is deferred, so resolveData() here serves only
 * products + broadcast nav.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkEmitRebase.hpp"   // Card 4 (M1) phase D: emit-layer frame re-base
#include "products/NetworkPbEmitFilter.hpp" // M5 prereq: PB emission-continuity filter
#include "products/NetworkProductIO.hpp"
#include "products/CombinedCorrectionSigma.hpp"   // M4: combined-correction σ arithmetic
#include "products/NetworkStateSnapshot.hpp"   // RTB-7: atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // Card 5 (RTF-1): broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/IlsCore.hpp"                 // producer-side network AR (Phase 2+): ils::ilsFix
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/NetworkCarryVce.hpp"        // M2: LS-VCE carry-row variance-component monitor
#include "filter/NetworkIonoDeweightGate.hpp" // Station-adaptive SH-VTEC-constraint deweight (iono deweight card)
#include "filter/NetworkInvariantCarryDriver.hpp"  // Card 4 (M1) C3b: DD cycle-basis invariant carry
#include "filter/NetworkMwWlLayer.hpp"             // M5 card WL-1: MW-seeded wide-lane pseudo-obs layer
#include "filter/StateKeySchema.hpp"
#include "filter/NetworkRunTypes.hpp"      // INC-2: run-loop record types (NetStation / StationAssembly / carry PODs)
#include "filter/NetworkRobustStats.hpp"   // INC-2: median/MAD robust attribution helpers
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "logging/NetworkRunTraces.hpp" // INC-3: trace-stream ownership + open logic + roster + FlagRate
#include "app/NetworkRunContext.hpp"    // INC-4: knobs + Tier-1 carry state + service counters (ctx)
#include "slip/MwLaneTiers.hpp"         // UWL-1 per-constellation lane-tier band table
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "slip/NetworkSlipQc.hpp"       // INC-2: slip-QC pure helpers (ifElevMap / parseIfPairResetKey)
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
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
#include "app/NetworkSetupResult.hpp"

namespace GnssProcessor {

int Application::networkValidateInputs_() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    // --- Phase 0: validate inputs (explicit RINEX + fixed coordinates) ------
    // V1 requires an explicit observation RINEX path per station: the
    // PathResolver network gate (filling paths from the internal data tree by
    // station name) is deferred to avoid editing the shared Application.cpp
    // resolve path while concurrent sessions are in flight.  Coordinates are
    // validated by loadNetworkSection_ (every station needs a known position).
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

    // Closed-loop integer-recovery harness: a HELD-OUT consumer station, added to
    // the sync roster ONLY so its observations are time-aligned with the network
    // schedule — it is deliberately kept OUT of the joint estimator stations
    // (built below) and instead consumes the producer's emitted SSR products.
    const ClosedLoopConfig& cl = nc.closed_loop;
    setup.closed_loop_on = cl.enable && !cl.station.name.empty();
    const bool closed_loop_on = setup.closed_loop_on;

    // Card 5 (RTF-1): a-priori orbit/clock source.  sp3 (default) = the precise
    // SP3/CLK cache is the a-priori (today's behaviour, byte-identical).  brdc_rts =
    // the a-priori is BROADCAST ephemeris (LiveEphemerisStore) — the self-sufficient
    // real-time service mode.  Threaded through Phase 3 (nav load + SP3-error lift),
    // the producer station gate-suppression (D1), the anchor-σ re-base (D6), the
    // clock-IODE stamp, and the apriori_product_id.  Every use is guarded on this
    // flag so the sp3 path is textually unchanged.
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

    // Clip each station's parse to the processing window.  The epoch loop below
    // is bounded by processing.max_duration_s (anchored on the first synced
    // epoch); without this, load() would parse — and synchronize() would copy —
    // the FULL 24h of every station even when only the first minutes are used,
    // which dominates wall-time at ~50 full-day stations.  A small margin covers
    // per-station first-epoch jitter; max_duration_s <= 0 keeps full-file parsing.
    //
    // The parse window is anchored at the file start, but network.start_offset_hours
    // fast-forwards the epoch loop PAST that offset before the max_duration cutoff
    // begins counting (the cutoff anchors on the first PROCESSED epoch).  So the
    // window must cover [0, start_offset + max_duration + margin]; clipping to only
    // max_duration would strip out the offset window and leave zero epochs to solve.
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
        // schedule (synchronize() skips it; the datum/pivot is guarded below in
        // buildStation).  This lets a large roster proceed when some stations'
        // observation files are missing or corrupt — the expected case when
        // fetching ~50 stations from a public archive.
        log->warn("Network: one or more station RINEX files failed to parse; "
                  "proceeding with the stations that loaded (the datum station "
                  "is required and is checked next).");
    }
    if (!sync.synchronize()) {
        log->error("Network: failed to synchronise epochs across stations");
        return 1;
    }
    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Network: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether the station files overlap in time");
        return 1;
    }
    log->info("Network: multi-station sync complete — {} synchronised epochs", synced.size());

    // --- Phase 3: products + cache for geometry -----------------------------
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Network: failed to initialise product cache — no broadcast nav / orbits");
        return 1;
    }
    log->info("Network: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    // Card 8 (replay parity): staged broadcast nav records + feed cursor for the
    // progressive LiveEphemerisStore insertion in the epoch loop (brdc_rts only;
    // empty otherwise).  Availability = GPS-continuous-scale toc (BDS toc is
    // native BDT → shifted to the GPS scale, mirroring LiveEphemerisStore's
    // toe convention).
    // navAvailabilityGpsSeconds: ONE shared inline fn in app/NetworkSetupResult.hpp
    // (INC-5) — the epoch-loop feed uses the identical definition.
    auto& brdc_nav_staged = setup.brdc_nav_staged;
    auto& brdc_nav_cursor = setup.brdc_nav_cursor;
    if (brdc_rts) {
        // Card 5 (RTF-1): the a-priori is BROADCAST ephemeris, not SP3/CLK.  Parse the
        // RINEX navigation file into the shared LiveEphemerisStore — the SAME store the
        // real-time engine fills from RTCM 1019/1042/1044/1046 — so replay resolves
        // satellite orbit+clock through the identical code path as a live run (frame
        // consistency, conventions §1 / plan D4).  Geometry::getSatellitePV falls back
        // to this store when no SP3 is loaded, and GnssCorrections::getSatelliteClock-
        // Correction supplies the broadcast SV clock (marked broadcast-grade) when no
        // precise CLK covers the epoch — exactly the broadcast a-priori this mode needs.
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (brdc_rts): network.apriori_source = brdc_rts requires a "
                       "broadcast navigation file, but files.navigation_path is empty. "
                       "Set it, or let PathResolver auto-resolve a BRDC / _MN.rnx file "
                       "covering the observation day.");
            return 1;
        }
        // Parse first so a per-constellation loaded-sat summary is available.
        //
        // Card 8 fix (replay parity, plan D4): do NOT bulk-insert the whole day.
        // LiveEphemerisStore retains only CURRENT+PREVIOUS per satellite (live
        // streaming semantics — the retention Cards 3/6/7 pin against), so a
        // full-file load leaves each satellite holding its LAST two records of
        // the day; at any processing epoch more than a fit window (~4 h GPS,
        // ~2 h GAL/BDS) before end-of-day the staleness guard then rejects
        // EVERY satellite and the producer degenerates to n_present = 0.
        // Instead STAGE the records sorted by availability and feed them to
        // the store progressively as the epoch loop advances, so the store's
        // current/previous evolve exactly as they would receiving a live nav
        // stream.  Availability proxy = GPS-scale toc: the file carries no
        // receipt stamp (RinexNavParser sets received_time = {0,0}), and toc
        // is the closest file-derivable stand-in for "a live receiver holds
        // this record" (live GPS broadcasts a data set from ~toe−2h, so the
        // toc proxy is conservative by up to one fit half-window — records
        // roll slightly LATER than live, never prematurely).
        std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        std::map<char, std::set<std::string>> nav_by_const;
        for (const auto& e : nav)
            if (!e.satellite.empty()) nav_by_const[e.satellite[0]].insert(e.satellite);
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
        // ANY precise product present in brdc_rts is an unsupported combination — refuse
        // rather than mask it (revisit when the RTS-orbit seam, plan D9, lands).  BOTH the
        // orbit AND the clock cache must be empty: a precise SP3 shadows the broadcast
        // a-priori via getSatellitePV node interpolation, and a precise CLK shadows the
        // broadcast clock in getSatelliteClockCorrection (interpolateClock wins over the
        // broadcast fallback for covered epochs → used_broadcast=false, NOT broadcast-grade).
        // Either alone is a silent orbit/clock frame split: the estimated C0 becomes a delta
        // against the wrong baseline while its stamped IODE still names the broadcast eph, so
        // the consumer reconstructs brdc(iode)+C0 and is metre-level wrong.  Gate on orbit OR
        // clock count (getOrbitCount alone missed a CLK-without-SP3 product set).
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
        // sp3 mode (default): the precise orbit cache IS the a-priori — unchanged.
        log->error("Network: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path / products to cover the observation day.");
        return 1;
    }

    // --- Phase 3b: GIM for satellite code-bias estimation (W6) ---------------
    // The satellite DCB (geometry-free code bias) is co-linear with each station's
    // free per-(rx,sat) slant ionosphere until an external constraint imposes
    // between-satellite structure.  When network.estimate_sat_code_bias is on,
    // load the IONEX GIM and fold a common-mode-free pseudo-observation per station
    // (see appendGimIonoRows); without it the satellite DCBs stay in the null space
    // and the rank gate withholds every epoch, so this is a hard prerequisite.
    // The satellite-DCB iono constraint comes from ONE of two mutually-exclusive
    // sources: the external GIM (legacy W6) or the co-estimated SH-VTEC field
    // (estimate_iono_sh).  They are never both applied — when the network estimates
    // its own field it does NOT load a GIM.
    auto& sh_model = setup.sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    if (config_.estimateNetworkIonoSh()) {
        sh_model = std::make_unique<SphericalHarmonicIonosphere>(shc.max_degree);
        log->info("Network (SH-iono): co-estimating own global SH-VTEC field — degree {} "
                  "({} coeffs), shell {:.0f} km, {} longitude; external GIM NOT loaded.",
                  sh_model->maxDegree(), sh_model->numCoeffs(), shc.layer_height_km,
                  shc.sun_fixed ? "sun-fixed" : "geographic");
        // Iono-2 mechanism A: second thin shell of the dual-layer field.
        if (shc.layer2_max_degree > 0) {
            setup.sh_model2 =
                std::make_unique<SphericalHarmonicIonosphere>(shc.layer2_max_degree);
            log->info("Network (Iono-2): dual-layer field — second shell degree {} "
                      "({} coeffs) at {:.0f} km; pin rows sum both shells.",
                      setup.sh_model2->maxDegree(), setup.sh_model2->numCoeffs(),
                      shc.layer2_height_km);
        }
        // T1a: echo the a₀₀ physical band the per-epoch divergence WARN uses.
        if (shc.a00_warn_low_tecu < shc.a00_warn_high_tecu)
            log->info("Network (SH-iono): a₀₀ physical band [{:.1f}, {:.1f}] TECU — a per-epoch "
                      "WARN fires if the estimated mean VTEC leaves it (field-divergence probe).",
                      shc.a00_warn_low_tecu, shc.a00_warn_high_tecu);
        else
            log->info("Network (SH-iono): a₀₀ physical-band WARN disabled (low ≥ high).");
    } else if (config_.estimateNetworkSatCodeBias() && config_.networkCodeBiasGfDatum()) {
        // SLANT posture (Odijk 2016 §5.1): the ionosphere stays free per-LOS slant and
        // is NOT constrained at all — no SH field, no GIM.  What makes the satellite
        // code bias observable here is not an ionosphere constraint but the third
        // frequency: rank deficiencies 2b/3b are removed by pinning the geometry-free
        // biases as datum (the GF pin + the receiver DCB gauge), which absorbs the GF
        // lump into the slant ionosphere (their Table 8) and leaves the modernized DCB
        // d^s_1 - d^s_j (j >= 3) as the estimable content.  Deliberately no GIM load and
        // no hard error — this branch exists so that combination is reachable at all.
        log->info("Network (W6/slant): NO ionosphere constraint by design — free per-LOS "
                  "slant delays with the geometry-free code biases pinned as datum "
                  "(network.code_bias_gf_datum).  The estimable satellite code bias is the "
                  "MODERNIZED DCB from the third frequency onward; the geometry-free lump "
                  "is absorbed by the ionosphere, so the emitted ionosphere and code biases "
                  "must be applied by a consumer AS A PAIR.");
    } else if (config_.estimateNetworkSatCodeBias()) {
        // Card 5 belt-and-braces: the external-GIM code-bias path is post-processing-only
        // (a GIM is a daily product, unavailable in the self-sufficient brdc_rts service).
        // Card 1's load-time validation rejects brdc_rts + estimate_sat_code_bias +
        // !estimate_iono_sh as a hard error, so reaching this branch in brdc_rts means the
        // guard was bypassed — refuse rather than silently loading a GIM the service can't have.
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
    // sparse channel (e.g. GPS L2S, ~13% of stations) is dropped EVERYWHERE rather
    // than mixed in only at the stations that happen to track it (the flaw of the
    // per-station channel_mode).  Every retained signal's satellite bias is then
    // estimated from a dense, consistent station set.  Coverage is tallied over the
    // synced epochs: a station "tracks" a code if it reports it for any satellite in
    // any epoch; coverage = (#stations tracking the code) / (#stations observing that
    // constellation).  Off (0) leaves the allowlist empty ⇒ every tracked channel is
    // admitted (byte-identical to the ungated producer).
    // Two independent gates share ONE coverage tally: signal_min_station_fraction
    // DROPS a sparse code (PppModel::setSignalAllowlist); phase_bias_min_station_
    // fraction instead FLOATS it — keeps the observation but withholds its satellite
    // phase-bias state (MeasurementModel::setPhaseBiasSignalAllowlist), so the float
    // ambiguity absorbs the bias and the signal is not AR-eligible.  Both default 0
    // (empty allowlists ⇒ byte-identical).
    std::map<std::string, std::set<std::string>> signal_allowlist;       // DROP gate (code + phase)
    std::map<std::string, std::set<std::string>> phase_bias_allowlist;   // FLOAT gate (phase only)
    const double sig_cover_min = config_.networkSignalMinStationFraction();
    const double pb_cover_min  = config_.networkPhaseBiasMinStationFraction();
    if (sig_cover_min > 0.0 || pb_cover_min > 0.0) {
        std::map<std::string, std::set<std::pair<std::string, std::string>>> sta_tracks; // station -> {(system, code)}
        for (const auto& se : synced) {
            for (const auto& [sta, sat_obs] : se.by_station) {
                if (sat_obs.empty()) continue;
                auto& trk = sta_tracks[sta];
                for (const auto& ob : sat_obs) {
                    const std::string sys = PppUtils::getSystemString(ob.system);
                    if (sys == "Unknown") continue;
                    for (const auto& [code, val] : ob.measurements) {
                        (void)val;
                        if (code.empty()) continue;
                        const char t = code[0];
                        if (t != 'C' && t != 'L') continue;   // code + phase channels only
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
            // Float gate applies to PHASE codes only (it withholds phase-bias states).
            if (pb_cover_min > 0.0 && key.second[0] == 'L' && frac >= pb_cover_min)
                phase_bias_allowlist[key.first].insert(key.second);
        }
        auto reportGate = [&](const char* label, double thr,
                              const std::map<std::string, std::set<std::string>>& allow,
                              bool phase_only) {
            if (thr <= 0.0) return;
            for (const auto& [sys, ns] : sys_n) {
                std::set<std::string> codes;
                for (const auto& [key, cnt] : code_n) {
                    (void)cnt;
                    if (key.first != sys) continue;
                    if (phase_only && key.second[0] != 'L') continue;
                    codes.insert(key.second);
                }
                std::string keep, gate;
                auto ait = allow.find(sys);
                for (const auto& c : codes) {
                    const std::string tag = fmt::format("{}({:.0f}%)", c, 100.0 * code_n[{sys, c}] / ns);
                    const bool admitted = ait != allow.end() && ait->second.count(c);
                    std::string& tgt = admitted ? keep : gate;
                    tgt += (tgt.empty() ? "" : " ") + tag;
                }
                log->info("Network {} gate [{}≥{:.0f}%]: {} stations observe {} — "
                          "KEEP [{}] | GATE [{}]", label, sys, 100.0 * thr, ns, sys, keep, gate);
            }
        };
        reportGate("signal-coverage (drop)", sig_cover_min, signal_allowlist, false);
        reportGate("phase-bias (float)",     pb_cover_min,  phase_bias_allowlist, true);
    }

    // --- Phase 4: per-station Kalman resources (pivot at index 0) -----------
    // Each station: a single-receiver StateManager (coords fixed at the
    // configured ECEF), PppModel bound to its RinexParser, and a MeasurementModel.
    // Pivot first so PASS A can seed its phase-signal set before the others.
    auto& stations = setup.stations;
    // Stations whose RINEX receiver antenna matched no loaded ATX entry (zero receiver
    // PCO/PCV applied).  Collected during buildStation for a single startup summary WARN
    // (the per-observation site logs only at trace, invisible at 305-station scale).
    std::vector<std::string> unmatched_antenna_stations;
    // Tier 1 (always-on radome→NONE fallback): stations recovered by the
    // type-preserving radome-agnostic NONE calibration (mm-level) — matched, but
    // via the fallback rather than an exact (type, radome) hit.  Reported once.
    std::vector<std::string> radome_fallback_stations;
    // Tier 2 (network.exclude_unmodeled_antenna_stations): stations dropped from
    // the network because their receiver antenna TYPE is unknown to the loaded
    // ANTEX even after the radome→NONE fallback (unmodelled receiver PCO/PCV).
    std::vector<std::string> excluded_unmodeled_antenna_stations;
    // buildStation outcome: Built (pushed to `stations`), ExcludedUnmodeledAntenna
    // (Tier 2 policy drop — counted, not built), or Failed (parser/load error).
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
        st.is_pivot  = is_pivot;
        st.sm        = std::make_unique<StateManager>(5, log_manager_, config_);  // pos(3)+clk(1)+tropo(1)
        Eigen::VectorXd x0 = st.sm->getStateVector();
        x0.head<3>() = st.ecef;
        st.sm->setStateVector(x0);
        st.sm->setStateEpochTime(synced.front().time);
        st.model = std::make_unique<PppModel>(
            config_, *product_parser_, log_manager_, *parser, *st.sm);
        // Network coverage gate: restrict this station's model to the codes that
        // enough of the whole network tracks (empty allowlist ⇒ no-op, admit all).
        if (!signal_allowlist.empty()) st.model->setSignalAllowlist(signal_allowlist);
        // Card 5 (RTF-1 / plan D1): in brdc_rts the a-priori satellite clock is
        // broadcast-grade, but the network ESTIMATES the clock (Δδtˢ vs that
        // a-priori), so this PRODUCER station's carrier phase is informative and
        // must be kept.  Suppress the code-only broadcast-phase gate on producer
        // models only — buildStation never builds the held-out closed-loop consumer
        // (the roster loop below skips it), so the consumer keeps the default gate.
        // sp3 mode passes false ⇒ byte-identical.
        st.model->setBroadcastPhaseGateSuppressed(brdc_rts);
        st.mm = std::make_unique<MeasurementModel>(
            *st.sm, *st.model, config_, log_manager_);
        // Phase-bias FLOAT gate: withhold satellite phase-bias states for signals
        // below network coverage (empty allowlist ⇒ no-op, every phase signal gets one).
        if (!phase_bias_allowlist.empty()) st.mm->setPhaseBiasSignalAllowlist(phase_bias_allowlist);
        // Seed this station's measurement-model frequency helper with its own
        // RINEX-header GLONASS FCN slots, so the GLONASS iono coefficient and
        // carrier wavelength in the Jacobian use the actual channel (not the
        // compiled-in default table).  Harmless for GPS/Galileo-only stations
        // (empty map).  Matters only under enableGlonassNetwork().
        st.mm->setGlonassSlots(parser->getGlonassFreqSlots());

        // Receiver-antenna ATX match check (startup diagnostic + optional phase-bias
        // exclusion).  A receiver antenna type that matches no loaded ANTEX entry gets
        // ZERO receiver PCO/PCV — getAntennaCorrection returns only the satellite-side
        // terms — a cm-level, elevation-dependent phase-centre error otherwise visible
        // ONLY at trace level (antenna.log is empty at default verbosity in a 305-station
        // run).  Checked once per station here; the per-observation trace still fires.
        // Only meaningful when a receiver correction would actually be applied.
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
            // radome→NONE fallback has already run, so a still-unmatched antenna is a
            // genuinely unknown TYPE with entirely unmodelled receiver PCO/PCV — a
            // systematic in every observation this station feeds into the shared
            // clocks/biases.  DROP it from the network (policy backstop, default OFF).
            if (config_.networkExcludeUnmodeledAntennaStations()) {
                // The clock-datum pivot realizes the datum and cannot be silently
                // dropped — surface it to the caller as a hard build error.
                if (is_pivot) return StationBuild::ExcludedUnmodeledAntenna;
                excluded_unmodeled_antenna_stations.push_back(cfg.name);
                log->warn("Network: station '{}' EXCLUDED from the network — unknown "
                          "receiver antenna TYPE (no ATX model; unmodelled receiver "
                          "PCO/PCV would be a systematic in the shared clocks/biases) "
                          "(network.exclude_unmodeled_antenna_stations).", cfg.name);
                return StationBuild::ExcludedUnmodeledAntenna;
            }

            // Kept (not hard-excluded) but unmatched → record for the loaded-station
            // summary WARN below.
            unmatched_antenna_stations.push_back(cfg.name);
            // Optional (network.phase_bias_exclude_unmatched_antenna): keep this station's
            // uncalibrated carrier phase out of the emitted satellite phase biases.  The
            // clock-datum pivot is NEVER excluded — it realizes the phase-bias datum, and
            // in the legacy single-pivot gauge its states ARE the shared set, so excluding
            // it would zero all emission; fix its antenna model instead.
            if (config_.networkPhaseBiasExcludeUnmatchedAntenna()) {
                if (is_pivot) {
                    log->warn("Network: clock-datum station '{}' has the unmatched antenna "
                              "but realizes the phase-bias datum — NOT excluded "
                              "(network.phase_bias_exclude_unmatched_antenna); supply its "
                              "ATX model to remove the receiver phase-centre bias.",
                              cfg.name);
                } else {
                    st.mm->setPhaseBiasEmissionExcluded(true);
                    log->info("Network: station '{}' excluded from phase-bias emission "
                              "contributions (unmatched receiver antenna).", cfg.name);
                }
            }
        } else if (rx_corr_on) {
            // Matched — but note when the match came only through the always-on
            // type-preserving radome→NONE fallback (mm-level radome-agnostic
            // calibration), so the substitution is visible once per station rather
            // than only at per-observation trace level.
            std::string detail;
            if (st.model->receiverAntennaViaRadomeFallback(&detail)) {
                log->warn("Network: station '{}' receiver antenna {}", cfg.name, detail);
                radome_fallback_stations.push_back(cfg.name);
            }
        }

        // Phase-leveling T0 (Increment 2): a persistent per-station MW + GF
        // TurboEdit detector pair for cycle-slip REPAIR of the carried ambiguities.
        // NON-pivot only on the legacy path — the single pivot's ambiguities are
        // ALL gauge-pinned and a repair there needs Increment 4 (design §6).  Under
        // the prioritized datum EVERY station (incl. the former pivot) carries the
        // arcs it does not anchor, so it MUST slip-detect them — build for all; the
        // PASS-A refloat still no-ops on a station's anchored (uncarried) arcs.
        // Constructed only when the carry AND a detector are on, so OFF is
        // byte-identical.  setCorrections(&this station's GnssCorrections) engages
        // the BIA-corrected wide-lane path (integer dN) where biases are available;
        // arc state lives on the detector and is station-local (parallel-safe).
        // M5 card WL-1: the MW-seeded wide-lane layer also needs the per-station MW
        // detector running (its per-arc Welford means feed the WL fold), and it must
        // work in the 3card lineage where phase_leveling is OFF.  Relax the gate to
        // build the detector on EVERY station when network.mw_wl.enable, independent of
        // phase_leveling / turbo_edit / mw_detect.  OFF (mw_wl disabled) is unchanged.
        // §9.7.10: datum_station_detect extends the pivot exemption — the datum
        // station needs the SAME MW detector rolled so its slips can be detected
        // at all (the response is a network re-reference, not a local refloat).
        const bool build_slip_for_pl =
            (config_.networkPhaseDatumPrioritized() || !is_pivot
             || config_.networkCycleSlipDatumStationDetect())
            && config_.networkPhaseLeveling()
            && (config_.networkCycleSlipTurboEdit() || config_.networkCycleSlipMwDetect());
        // Adaptive iono q (network.adaptive_iono_q): per-station GF-TD2 variability
        // monitor.  Station-local (fed in the PARALLEL PASS A produce, same contract
        // as st.mw_slip below); consumed SERIALLY by the T5v2 GM deviation aging —
        // on the network path the per-(station,satellite) deviation channel is the
        // only time-linked slant-iono element, so the storm response scales ITS
        // innovation term, not a slant q.  OFF ⇒ null pointer, byte-identical.
        if (config_.getNetworkConfig().adaptive_iono_q)
            st.iono_var = std::make_unique<IonoVariabilityMonitor>(config_, log_manager_);

        // M5 card WL-2 (SPEC_ewl_wl_cascade_card.md §4): the EWL/WL lanes also need the
        // per-station MW detector running (its per-arc Welford means feed the EWL/MW(1,2)
        // statistics).  OFF (all lanes disabled) ⇒ unchanged.
        const auto& mwc = config_.getNetworkConfig().mw_wl;
        const auto& lcc = config_.getNetworkConfig().lane_carry;   // M5 WL-3
        if (build_slip_for_pl || config_.networkMwWlSeed()
            || mwc.ewl_enable || mwc.wl_inject_enable || lcc.enable) {
            st.mw_slip = std::make_unique<MwSlipDetector>(config_, log_manager_);
            st.mw_slip->setCorrections(&st.model->getCorrections());
            // M5 card WL-2 (SPEC §3.4): arm the tri-frequency GF_b phase bridge accumulator
            // only when a consumer of the bridge exists — the WL-2 inject path or the WL-3
            // lane bridge increments — together with the EWL integers it bridges from.
            // OFF ⇒ the detector never touches a bridge arc (byte-identical).
            st.mw_slip->setBridgeEnabled(
                (mwc.wl_inject_enable || (lcc.enable && lcc.bridge_meas)) && mwc.ewl_enable);
            // 60-ep age-bias fix: per-sample estimable-iono de-bias of the bridge
            // accumulator (frame contract in the setter's doxygen).  Armed only with
            // the bridge itself; the per-epoch ı̃ push rides the iono_of build below.
            st.mw_slip->setBridgePerSampleDebias(
                (mwc.wl_inject_enable || (lcc.enable && lcc.bridge_meas)) && mwc.ewl_enable
                && mwc.bridge_sample_debias);
            // M5 card UWL-1 (SPEC_uwl_card.md §3.1): push the per-constellation lane-tier
            // band digits for the bridge band pick.  Not pushed (empty) unless the card is
            // armed ⇒ the legacy {1,2,5} scan for every constellation, byte-identical —
            // including under multi-GNSS active_systems (§6.1(b)).  With the table pushed,
            // a constellation ABSENT from it (BDS-3, deferred on its clock divergence)
            // forms NO bridge arc; the wiring warns loudly instead of skipping silently.
            if (mwc.uwl_enable) {
                std::map<char, MwLaneTier> tiers;
                for (const char cons : { 'G', 'E' })
                    if (const auto t = mwLaneTierOf(cons, mwc.uwl_gal_top_band,
                                                    mwc.uwl_mid_enable, mwc.uwl_dual_top))
                        tiers[cons] = *t;
                st.mw_slip->setLaneTiers(std::move(tiers));
            }
            // The TurboEdit MW+GF integer REPAIR resolver is built ONLY for turbo_edit
            // (higher-rate 1 s data, where the geometry-free ionospheric drift over the
            // epoch gap is small enough to trust an integer).  mw_detect uses the SAME MW
            // detector as a pure DETECTOR → refloat, so it needs no resolver — and the null
            // st.turbo_edit is exactly what steers the PASS-A branch to the refloat path.
            if (config_.networkCycleSlipTurboEdit()) {
                st.turbo_edit = std::make_unique<TurboEditResolver>(config_, log_manager_);
                st.turbo_edit->setCorrections(&st.model->getCorrections());
            }
        }

        // IF-phase triple-difference slip DETECTION (this increment): a persistent
        // per-station between-satellite/between-epoch ionosphere-free phase OMC
        // detector.  NON-pivot only on the legacy path; under the prioritized datum
        // build for EVERY station (the former pivot carries non-anchored arcs that
        // need detection).  Constructed whenever network.cycle_slip.if_phase_td is
        // on — independent of phase-leveling, so it can run as a pure diagnostic in
        // the absolute solve; the refloat ACTION it drives is gated on the carry
        // being active at the call site.  Station-local detector state (parallel-safe).
        if ((config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkCycleSlipIfPhaseTd()) {
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);
            st.if_slip->setDeferFlagging(true);  // network pools per-pair MAD; the driver flags
            // Card R (docs/SPEC_slip_qc_hardening.md §2): push the hardened
            // reference-satellite gate.  Pushed ONLY here, so the STAGED
            // user/kinematic path keeps the legacy gate untouched (spec R5); with
            // network.cycle_slip.if_phase_td_ref_gate off the params are the
            // default-constructed (disabled) ones ⇒ byte-identical.
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
                st.if_slip->setRefGate(rg);
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
                  "(receiver PCO/PCV zeroed there): {}{}",
                  unmatched_antenna_stations.size(), stations.size(), names,
                  config_.networkPhaseBiasExcludeUnmatchedAntenna()
                      ? " — non-pivot ones excluded from phase-bias emission"
                      : " — supply their ATX models, or set "
                        "network.phase_bias_exclude_unmatched_antenna to withhold them");
    }
    // Join a name vector into a comma-separated list for the summaries below.
    auto joinNames = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s;
    };
    // Tier 1 (always-on radome→NONE): report the stations whose receiver PCO/PCV
    // came from the radome-agnostic NONE calibration (mm-level) — matched, not
    // dropped, but worth a single summary so the substitution is visible.
    if (!radome_fallback_stations.empty()) {
        log->info("Network: {} of {} loaded station(s) matched their receiver antenna via "
                  "the type-preserving radome→NONE fallback (radome uncalibrated; NONE "
                  "PCO/PCV applied, mm-level): {}",
                  radome_fallback_stations.size(), stations.size(),
                  joinNames(radome_fallback_stations));
    }
    // Tier 2 (network.exclude_unmodeled_antenna_stations): run-end counter for the
    // stations dropped because their receiver antenna TYPE is unknown to the ANTEX
    // even after the radome→NONE fallback (invariant #4 — ON-path telemetry).
    if (!excluded_unmodeled_antenna_stations.empty()) {
        log->warn("Network: {} station(s) excluded (unmodeled receiver antenna): {}",
                  excluded_unmodeled_antenna_stations.size(),
                  joinNames(excluded_unmodeled_antenna_stations));
    } else if (config_.networkExcludeUnmodeledAntennaStations()) {
        log->info("Network: 0 stations excluded (unmodeled receiver antenna) — every "
                  "loaded station's receiver antenna resolved (radome→NONE fallback "
                  "covered any uncalibrated radomes).");
    }

    // --- Station roster (viz) -----------------------------------------------
    // A tiny static CSV of the stations that actually loaded (id, role, ECEF) so
    // the pyviz worldmap reflects the real run, not just the config.  The
    // held-out closed-loop consumer (if any) is listed with role "held_out".
    if (nc.trace_stations)
        NetworkRunTraces::writeStationRoster(log_dir_, stations,
                                             closed_loop_on ? &cl.station : nullptr, log);

    // === Per-station compute parallelism ====================================
    // PASS A (geometry) and the produce half of PASS B (measurement assembly) are
    // embarrassingly parallel across stations — each owns an independent
    // StateManager / PppModel / MeasurementModel, and the audited call trees
    // touch no shared mutable state (product stores are const post-load; the
    // per-instance model caches are thread-private; the live SSR/ephemeris
    // singletons are mutex-guarded and empty in post-processing).  Only the
    // shared-estimator scatter (est.addStation) and everything downstream of it
    // stays serial, in deterministic station order, so the float solution is
    // BIT-IDENTICAL to a serial run regardless of thread count.  Activated only
    // above the configured station-count threshold (small networks run serially —
    // no fork-join cost, and the internal 3-station fixture is unchanged).
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
    // DD carry-fold + D5 innovation-gate readouts.  Rides on the per-station parallel
    // path (same thread budget, Eigen already pinned to one thread for the epoch) and,
    // unlike parallel_reduce, is BYTE-IDENTICAL at any thread count (compute per block
    // in parallel, accumulate/install serially in fixed block order).  seam_threads == 1
    // ⇒ the seam runs its original serial path (OFF is byte-for-byte the pre-seam run).
    const bool seam_on      = parallel_on && nc.parallel_seam;
    setup.seam_threads = seam_on ? n_threads : 1;
    const int  seam_threads = setup.seam_threads;
    if (seam_on)
        log->info("Network: post-solve carry-fold/D5 seam PARALLEL across {} threads "
                  "(deterministic — byte-identical to the serial fold)", n_threads);
    // network.parallel_carry: thread the Card-4 carry's dense per-block algebra
    // (buildDdPriors W·Pᶻ·Wᵀ, foldReducedDdPriors, installPosterior) via Eigen's own
    // multi-threaded GEMM.  The seam only parallelizes the carry over the ~few blocks;
    // this un-pins Eigen so each block's O(K²–K³) matmul fans across cores.  Runs those
    // stages SERIAL-over-blocks (n_threads=1) so Eigen threading does not nest with the
    // seam's OpenMP.  Reassociates the dense products ⇒ ~1e-X ULP drift vs serial (like
    // parallel_reduce); default OFF keeps Eigen pinned and the seam/serial path (byte-id).
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
    // first-touch CREATE path mutates a shared std::map; fetching an existing
    // logger is lock-free and data-race-safe.  Creating each one now (single-
    // threaded) guarantees no insert ever races a concurrent fetch inside the
    // parallel region — the only shared-mutable hazard the thread-safety audit
    // found in the per-station call tree.
    //
    // INC-7 re-audit: the slip detectors' loggers ("mw_slip_detector",
    // "if_slip_detector") are deliberately NOT on this list — they are
    // first-touch-created by the detector CONSTRUCTORS in this (serial) setup
    // phase above, before any parallel region runs, so the create-race cannot
    // occur.  If detector construction ever moves out of the serial setup,
    // add them here.
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
    // A2: pass the clock-disc toggle so the emitter maintains + stamps the per-sat clock
    // discontinuity counter.  R9: the phase-bias disc threshold is the emitter's own default
    // constant (single source of truth shared with the serialized contract's disc_threshold_m).
    // RTA-11: the staleness horizon (product-time seconds; 0 = off) makes a key returning
    // after a longer gap a first sighting (no spurious disc bump against an hours-stale
    // baseline); like the other two parameters it travels in the serialized contract.
    setup.emitter.emplace(NetworkProductEmitter::kDefaultDiscThresholdM,
                          config_.networkEmitClockDisc(),
                          config_.networkEmitterStalenessHorizonS(),
                          config_.networkCodeBiasDiscJumpTest(),
                          config_.networkClockDiscOnSetChange(),
                          config_.networkDiscThresholdFracCycles());
    NetworkProductEmitter& emitter = *setup.emitter;

    // --- Optional on-disk product serialization -----------------------------
    // A reusable per-epoch dump of the emitted solution (the same pre-emit
    // NetworkSolutionEpoch the in-memory closed loop consumes, plus this epoch's
    // datum-change flag).  Lets consumer-side experiments — notably the consumer
    // AR-frame work — replay the products WITHOUT re-running the multi-station
    // network: NetworkProductReader::replay reproduces this store byte-for-byte
    // (discontinuity counters included).
    auto& product_writer = setup.product_writer;
    auto& prod_path = setup.prod_path;
    // B6 (P1-1): when the S-basis contract is serialized, DEFER opening the writer until
    // the first emit so the realized per-constellation clock pivots (auto-picked, unknown
    // until the first solve) can be stamped into the v2 header — not just the config
    // intent.  pending_contract holds everything known up front; realized pivots merge in
    // at the first emit.  contract_pending gates that one-shot construction.
    auto& pending_contract = setup.pending_contract;
    auto& contract_pending = setup.contract_pending;
    if (!nc.product_output_path.empty()) {
        // A relative path lands under the network trace dir, alongside
        // network_solution.csv etc.; an absolute path is used verbatim.
        prod_path = nc.product_output_path;
        if (prod_path.is_relative()) prod_path = log_dir_ / prod_path;
        if (nc.serialize_sbasis_contract) {
            // B6: stamp the S-basis contract into a v2 header.  Declaration semantics are
            // the A3-corrected per-constellation sat-clock-pin basis.
            pending_contract.decl.reference_receiver_id = nc.clock_datum_station;
            pending_contract.decl.emit_code_bias        = config_.estimateNetworkSatCodeBias();
            // T3b: self-describe the phase-bias gauge in the on-disk contract too.  Under the
            // anchor-free ensemble datum the emitted bias references the receiver ENSEMBLE, not
            // a pivot receiver; the runtime per-solve decl already flips (see ~AmbDatum below),
            // but the startup contract omitted it and mis-declared the default pivot frame.
            // Mirrors the isb_datum self-description that follows.  OFF ⇒ default ⇒ byte-identical.
            if (config_.networkPhaseDatumEnsemble())
                pending_contract.decl.amb_datum =
                    SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
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
                // (Local clock_sum_to_zero is declared further down; the getter is
                // cheap and this one-shot startup block precedes it.)
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
            } else if (!nc.clock_datum_satellite.empty()) {
                pending_contract.decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                    = nc.clock_datum_satellite;
            }
            // P1-1: resolve the a-priori orbit/clock identity to the ACTUAL resolved
            // SP3 + CLK file basenames (the baseline the emitted clock C0 is a delta
            // against).  Config intent alone is "CODFIN00000000" under auto-detect.
            const auto& dl = config_.getDownloadConfig();
            std::string apriori_id =
                fmt::format("{}{}{:04d}{:03d}", dl.product_source, dl.product_type, dl.year, dl.doy);
            if (brdc_rts) {
                // Card 5 (plan D10): the a-priori is broadcast ephemeris, so the identity
                // is the nav-file basename ("brdc:<file>").  Once an RTS-orbit source is
                // attached (plan D9 seam / RTF-3), this becomes "brdc+rts:<mount>".
                const std::string nav = config_.getNavigationPath().filename().string();
                apriori_id = "brdc:" + (nav.empty() ? std::string("?") : nav);
            } else {
                const std::string sp3 = config_.getProductPath("SP3").filename().string();
                const std::string clk = config_.getProductPath("CLK").filename().string();
                if (!sp3.empty() || !clk.empty())
                    apriori_id = (sp3.empty() ? std::string("?") : sp3) + "+"
                               + (clk.empty() ? std::string("?") : clk);
            }
            pending_contract.apriori_product_id = apriori_id;
            // RTA-12: contract identity fields (each serialized as an optional
            // key=value S-line field ONLY when non-empty).
            //  - frame_id / coord_epoch are config DECLARATIONS (network.frame_id /
            //    network.coord_epoch; the SIF-coordinate path defaults coord_epoch
            //    to the propagation date) — no auto-detection.
            //  - atx_id is the RESOLVED antenna-calibration file this run actually
            //    loaded (basename of the merged product path).  Failure mode: when
            //    no .atx resolved, getProductPath returns an empty path and the
            //    atx= field is simply absent — a consumer that pins expect_atx_id
            //    then flags "file '' vs expected ...".
            pending_contract.frame_id    = nc.frame_id;
            pending_contract.coord_epoch = nc.coord_epoch;
            pending_contract.atx_id      = config_.getProductPath("ATX").filename().string();
            //  - orbit_ref: SP3 orbits + ATX satellite PCO/PCV mean the emitted
            //    clock C0 is referred to the satellite CENTER OF MASS.  Hardcoded
            //    until an SSR orbit source (APC-referred, per the RTCM SSR
            //    convention) lands, at which point this becomes a variable of the
            //    orbit path selection.
            pending_contract.orbit_ref   = "CoM";
            pending_contract.disc_threshold_m   = NetworkProductEmitter::kDefaultDiscThresholdM;  // R9: same constant as the emitter above
            // The replay-emitter construction parameters (disc threshold above + the
            // clock-disc channel flag + the RTA-11 staleness horizon) travel in the
            // contract so a consumer can replay the file with byte-identical
            // discontinuity counters and no out-of-band knowledge of the producer's
            // config.  The horizon serializes as an optional stale= field only when
            // non-zero, so an OFF run's v2 header is byte-identical.
            pending_contract.emit_clock_disc    = config_.networkEmitClockDisc();
            pending_contract.staleness_horizon_s = config_.networkEmitterStalenessHorizonS();
            pending_contract.code_bias_jump_test = config_.networkCodeBiasDiscJumpTest();
            pending_contract.clock_disc_on_set_change = config_.networkClockDiscOnSetChange();
            pending_contract.disc_threshold_frac_cycles = config_.networkDiscThresholdFracCycles();  // H3: per-λ jump gate (discfrac=)
            // M4 (contract v2): advertise the combined-σ age-scaling coefficient on the
            // S line ONLY when the combined-σ channel is on and a coefficient is set, so
            // an OFF (or agek=0) run's v2 header is byte-identical (agek= token absent).
            if (config_.networkEmitCombinedSigma())
                pending_contract.age_scale_k = config_.networkCombinedSigmaAgek();
            // H-rider: advertise which statistic the H heartbeat's sigma0 carries.
            // ONLY when the census feed is armed (hsig= token absent otherwise,
            // byte-identical) — the census σ̂₀ is a different population from the
            // within-epoch RAIM's, and a consumer thresholding field 5 must know.
            if (config_.networkHSigma0PrefitCensus())
                pending_contract.h_sigma0_source = "pc";
            pending_contract.present            = true;
            contract_pending = true;
            // The deferred writer only opens (and truncates) at the FIRST emit; a run
            // that emits zero epochs would otherwise leave a PREVIOUS run's complete
            // product file at this path for a downstream consumer to silently replay.
            // Match the v1 branch's truncate-at-startup by removing any existing file.
            std::error_code stale_ec;
            std::filesystem::remove(prod_path, stale_ec);
            if (stale_ec && std::filesystem::exists(prod_path))
                log->warn("Network: could not remove a stale product file at '{}' ({}) — "
                          "if this run emits no epochs, a downstream consumer would replay "
                          "the PREVIOUS run's products from that path.",
                          prod_path.string(), stale_ec.message());
            log->info("Network: will serialize per-epoch products with a v2 S-basis contract "
                      "(realized pivots stamped at first emit) to '{}'", prod_path.string());
        } else {
            product_writer = std::make_unique<NetworkProductWriter>(prod_path.string());
            if (product_writer->good()) {
                log->info("Network: serializing per-epoch products to '{}'", prod_path.string());
            } else {
                log->error("Network: failed to open product_output_path '{}' for writing; "
                           "disabling on-disk serialization for this run.", prod_path.string());
                product_writer.reset();
            }
        }
    }

    // --- Closed-loop consumer (held-out station) ----------------------------
    // The held-out station runs the user-side PPP-RTK + AR pipeline through a
    // RealTimeEngine fed epoch-by-epoch from RINEX replay, with its SSR
    // consumption repointed at the OWNED network_store (never the singleton).
    // Each epoch: the producer emits into network_store, then the consumer reads
    // that fresh epoch — closing the loop "does our own user engine recover
    // integers from our own products on an independent station?".
    setup.consumer_config = config_.deriveClosedLoopConsumerConfig();
    GnssConfig& consumer_config = setup.consumer_config;
    auto& consumer = setup.consumer;
    setup.cl_truth_arp = Eigen::Vector3d(cl.station.position_ecef.data());
    if (closed_loop_on) {
        // Reduce the configured (monument-referenced) truth into the ARP frame the
        // consumer estimates in, using the held-out station's own RINEX header.  A
        // zero eccentricity leaves the value bit-identical, so every station in the
        // current fixture set (CIT1, NRMG: 0/0/0) grades exactly as before.
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
        // per-epoch observations themselves come from the synced roster below.
        consumer_config.setObservationPath(cl.station.rinex_path);
        consumer = std::make_unique<RealTimeEngine>(consumer_config, log_manager_);
        if (!consumer->initialize()) {
            log->error("Network closed-loop: consumer engine initialize() failed; "
                       "disabling the harness for this run.");
            consumer.reset();
        } else {
            // Direct LAMBDA on the network-bias-corrected float must NOT also take
            // the MW WL (it is in the .BIA frame; see setArIgnoreMwWl).  wl_only AR
            // keeps the MW WL by definition.
            consumer->setArIgnoreMwWl(!consumer_config.getArWlOnly());
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
                // Consume OUR products (never the singleton).  consume_network_clock
                // gates the clock overlay: false keeps precise SP3/CLK and consumes
                // only the network phase bias (the clean integer-recovery test).
                consumer->setSsrStore(&network_store, cl.consume_network_clock, /*bias=*/true);
                // Strong (ionosphere-corrected) user model: consume the network's
                // SH-VTEC slant iono as a tight per-sat BLUP prior (the producer
                // fills the store's slant-iono channel each epoch below).  Default
                // OFF ⇒ the consumer floats its own iono (byte-identical).
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

} // namespace GnssProcessor
