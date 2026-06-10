/**
 * @file ApplicationBaseline.cpp
 * @brief Implementation of Application::runBaseline (DD baseline processing).
 *
 * Extracted from Application.cpp to keep the per-mode processing
 * pipelines in dedicated translation units.  The class definition,
 * constructor, shared helpers, and the PPP/PPP-RTK
 * @c Application::run() entry point still live in Application.cpp.
 */

#include "app/Application.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "math/TimeUtils.hpp"
#include "products/ProductCache.hpp"
#include "math/Coordinates.hpp"
#include "products/ProductResolver.hpp"
#include "realtime/NtripClient.hpp"
#include "realtime/RtcmStreamReader.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "filter/StateKeySchema.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "slip/MwSlipDetector.hpp"
#include <chrono>
#include <set>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {

int Application::runBaseline() {
    auto log = log_manager_.getLogger("main_log");
    const auto& bc_initial = config_.getBaselineConfig();
    const bool is_multi = (config_.getProcessingMode() == ProcessingMode::BaselineMulti);

    log->info("Baseline processing requested (mode={})",
        is_multi ? "Baseline-Multi" : "Baseline-Single");

    // --- Phase 0: PathResolver pass --------------------------------
    // Baseline mode bypasses run()'s resolveData() call (the dispatch
    // happens before that step), so we run the resolver here.  In
    // internal mode this fills in any missing baseline.{reference,rovers}
    // .rinex_path from the bundled data/observations/ tree by station-
    // name match.  In production mode it downloads navigation + products
    // (using the reference station's RINEX as the date anchor — the
    // current ProductResolver builds URLs from year/doy, not from the
    // baseline obs files themselves).
    if (config_.isProductionDataSource() && config_.getDownloadConfig().enabled
        && !bc_initial.reference.rinex_path.empty()) {
        // Same hack as before: point the observation_path slot at the
        // reference station so any future date-inference logic in
        // ProductResolver picks the right day.
        config_.setObservationPath(bc_initial.reference.rinex_path);
    }
    if (!resolveData()) {
        log->error("Baseline: PathResolver failed");
        return 1;
    }

    // Re-read the (possibly updated) baseline config after resolution.
    const auto& bc = config_.getBaselineConfig();
    log->info("  Reference: '{}' rinex={} ECEF=({:.3f}, {:.3f}, {:.3f}) m ({})",
        bc.reference.name,
        bc.reference.rinex_path.string(),
        bc.reference.position_ecef[0],
        bc.reference.position_ecef[1],
        bc.reference.position_ecef[2],
        bc.reference.fixed ? "fixed" : "estimated");
    for (const auto& rv : bc.rovers) {
        log->info("  Rover:     '{}' rinex={}{}",
            rv.name, rv.rinex_path.string(),
            rv.has_position ? " (a-priori position supplied)" : "");
    }

    // Hard-fail early if PathResolver could not produce a usable set
    // of inputs.  These checks moved here from loadBaselineSection_:
    // the config loader can no longer reject empty rinex_path because
    // PathResolver is the legitimate source of those values in
    // internal mode.  Failing here, after resolution, gives a clear
    // diagnosis with the resolved-or-empty path quoted in the message.
    if (bc.reference.rinex_path.empty()) {
        log->error("Baseline: reference station '{}' has no rinex_path "
                   "after resolution. In internal mode the resolver scans "
                   "data/observations/ for a filename containing '{}' "
                   "(case-insensitive) — make sure such a file exists, "
                   "or set baseline.reference.rinex_path explicitly.",
                   bc.reference.name, bc.reference.name);
        return 1;
    }
    for (const auto& rv : bc.rovers) {
        if (rv.rinex_path.empty()) {
            log->error("Baseline: rover '{}' has no rinex_path after resolution. "
                       "Same advice as the reference: drop a matching file under "
                       "data/observations/ or set baseline.rover[s].rinex_path "
                       "explicitly.", rv.name);
            return 1;
        }
    }
    if (config_.getNavigationPath().empty()) {
        log->warn("Baseline: no navigation file resolved. "
                  "Internal mode expects data/products/brdc/unzip/*.rnx; "
                  "production mode requires data_download.enabled=true.");
    }

    // --- Phase 1: multi-station RINEX ingestion + epoch alignment (slice 2) ---
    // Baseline mode dispatches before loadInputData() (which is the normal
    // creation site for product_parser_), so construct one here.  Products
    // are loaded below; the DD processing cancels satellite-side errors,
    // so precise orbits/clocks/biases aren't strictly required for short
    // baselines but broadcast nav IS needed for satellite position lookup.
    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }
    MultiRinexSync sync(config_, bc, log_manager_, *product_parser_);
    if (!sync.load()) {
        log->error("Baseline: failed to load one or more RINEX observation files");
        return 1;
    }
    if (!sync.synchronize()) {
        log->error("Baseline: failed to synchronise epochs across stations");
        return 1;
    }

    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Baseline: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether station files overlap in time");
        return 1;
    }
    log->info("Baseline: multi-station sync complete — {} synchronised epochs ready for DD processing",
              synced.size());

    // --- Phase 2: Load products + product cache for geometry (slice 5) ---
    // Broadcast nav (config's navigation_path) is required to compute
    // satellite positions at observation times.  Precise products (SP3,
    // CLK, BIA) are not strictly needed for short-baseline DD — satellite
    // clocks and biases cancel between receivers — but loading them is
    // harmless if present.
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Baseline: failed to initialise product cache — no broadcast nav / orbits");
        return 1;
    }
    log->info("Baseline: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    if (product_cache.getOrbitCount() == 0) {
        log->error("Baseline: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path to a valid RINEX nav file in the config.");
        return 1;
    }

    // ===================================================================
    // SHARED REFERENCE STATION SETUP (once, outside the rover loop)
    //
    // BaselineMulti uses a star topology: each rover is processed
    // independently against the same master reference.  The reference's
    // PppModel state, ECEF, and MW arc detector are constructed once
    // here; their per-rover state (ref_ppp_state_ values, mw_ref running
    // statistics) is RESET at the top of each rover iteration so the
    // sequential loop sees a clean reference each time.
    // ===================================================================
    const RinexParser* ref_parser = sync.parserOf(bc.reference.name);
    if (!ref_parser) {
        log->error("Baseline: reference station parser unavailable");
        return 1;
    }
    const Eigen::Vector3d ref_pos(bc.reference.position_ecef.data());

    ref_state_manager_ = std::make_unique<StateManager>(3, log_manager_, config_);
    {
        Eigen::VectorXd x0 = ref_state_manager_->getStateVector();
        x0.head<3>() = ref_pos;
        ref_state_manager_->setStateVector(x0);
        ref_state_manager_->setStateEpochTime(synced.front().time);
    }
    ref_ppp_model_ = std::make_unique<PppModel>(
        config_, *product_parser_, log_manager_, *ref_parser, *ref_state_manager_);

    // Optional GIM-backed higher-order ionosphere correction.  Baseline
    // (DD) processing differences the higher-order term between rover
    // and reference, so the practical benefit is tiny on short baselines —
    // expose the wiring for consistency with PPP/PPP-RTK and for long
    // baselines where I₂ stops cancelling cleanly.  The same gim_
    // instance is shared by ref and per-rover PppModels (set on the
    // rover model where it's constructed inside the rover loop).
    if (config_.getModelCorrections().enable_higher_order_ionosphere && !gim_) {
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log_manager_.getLogger("ionosphere")->warn(
                "Baseline mode: higher_order_ionosphere is enabled but no "
                "usable IONEX GIM was resolved — the correction will be skipped.");
        }
    }
    if (gim_ && gim_->isLoaded()) {
        ref_ppp_model_->setGim(gim_.get());
    }

    MwSlipDetector mw_ref(config_, log_manager_);
    if (config_.getLoggingConfig().enable_mw_slip_trace) {
        mw_ref.enableTracing(log_dir_);
    }

    // Build the active-systems set once.  The MW pre-filter must respect
    // the same active_systems gate that the DD measurement model uses
    // downstream; otherwise MW slips on systems the user explicitly
    // disabled (e.g. "active_systems": ["GPS"]) would still erase
    // ambiguities for unrelated constellations and pollute the slip stats.
    const auto& active_vec = config_.getActiveSystems();
    const std::set<std::string> active_set(active_vec.begin(), active_vec.end());

    // Lambda: convert a station's epoch RINEX observations into the
    // minimum PppObservation subset the MW detector needs.  MW itself
    // is a raw code+phase combination — geometry (sat_pos, LOS, range,
    // tropo, etc.) is irrelevant — but @c MwSlipDetector::detectLargeMwResets
    // gates each observation by @c obs.elevation against the configured
    // elevation mask, so populating elevation is required or every obs
    // is silently skipped.  We use the PppModel's orbit interpolator
    // (already loaded from product or broadcast sources) to get sat
    // position at the receive epoch and compute topocentric elevation
    // against the station's static a-priori ECEF.  An unresolvable
    // satellite leaves elevation at zero and the mask drops it — the
    // same outcome the legacy code path produced for missing orbits.
    // Populating both @c observed and @c raw_observed lets either
    // @c MwFrequencyMode (Mirror or Dual/Triple/All) work.  Observations from systems not
    // in @c active_systems are dropped here so the MW detector never
    // sees them.  Stateless and shared across all rovers; per-rover /
    // per-reference Geometry handle and active_set are supplied at the
    // call site.
    auto toMinimalPppObs = [](const std::vector<RinexObservation>& rin_obs,
                              const Eigen::Vector3d& rx_pos,
                              const Model::Geometry& geom,
                              const std::set<std::string>& active_systems) {
        std::vector<PppObservation> out;
        out.reserve(rin_obs.size());
        for (const auto& r : rin_obs) {
            // Active-systems gate: skip observations from disabled
            // constellations.  Empty active_systems is treated as "all
            // active" to preserve legacy behavior when the field is
            // unset.
            if (!active_systems.empty()) {
                const std::string cons = PppUtils::getSystemString(r.system);
                if (!active_systems.count(cons)) continue;
            }
            PppObservation p{};
            p.satellite  = r.satellite;
            p.system     = r.system;
            p.time       = r.time;
            p.lli_flags  = r.lli_flags;
            for (const auto& [code, val] : r.measurements) {
                if (code.empty()) continue;
                const char t = code[0];
                if (t != 'C' && t != 'L') continue;  // MW uses code + phase only
                p.observed[code]     = val;
                p.raw_observed[code] = val;
            }
            if (!r.satellite.empty() && rx_pos.norm() > 1.0) {
                const auto pv = geom.getSatellitePV(r.satellite, r.time);
                if (pv.first.norm() > 1.0) {
                    const auto topo = geom.computeTopocentric(rx_pos, pv.first);
                    p.elevation = topo.elevation_deg;
                    p.azimuth   = topo.azimuth_deg;
                    p.sat_pos   = pv.first;
                }
            }
            out.push_back(std::move(p));
        }
        return out;
    };

    log->info("Baseline-Multi orchestrator: {} rover(s) to process against reference '{}'",
              bc.rovers.size(), bc.reference.name);

    // ===================================================================
    // PER-ROVER LOOP
    //
    // Sequential processing: each rover gets a freshly-built filter,
    // measurement model, MW detector, and AR resolver.  Trace files for
    // multi-rover runs land in log_dir_ / "rover_<name>" subdirectories
    // so they don't collide; single-rover runs keep the legacy
    // log_dir_ / "baseline_trace.csv" path for backward compatibility.
    // The shared reference resources above are reset between rovers.
    // ===================================================================
    const bool multi_rover = (bc.rovers.size() > 1);
    size_t total_rover_updates = 0;
    size_t total_rover_dd_obs  = 0;

    for (size_t rover_idx = 0; rover_idx < bc.rovers.size(); ++rover_idx) {
        const auto& rover_cfg = bc.rovers[rover_idx];
        const std::string& rover_name = rover_cfg.name;

        log->info("=== Baseline rover {}/{}: '{}' ===",
            rover_idx + 1, bc.rovers.size(), rover_name);

        // Reset shared reference state for this rover.  Without this the
        // second rover's MW detector inherits arc statistics from the
        // first rover's run, the ref_ppp_state_ epoch points at the last
        // processed epoch from the previous rover, and the sticky DD
        // pivot manager retains stale entries.  Each rover must start
        // from the same well-defined reference state.
        ref_ppp_state_.position = ref_pos;
        ref_ppp_state_.clock_bias = 0.0;
        ref_ppp_state_.troposphere_zwd = 0.0;
        ref_ppp_state_.epoch_time = synced.front().time;
        if (rover_idx > 0) {
            mw_ref.resetAll();
        }

        const RinexParser* rov_parser = sync.parserOf(rover_name);
        if (!rov_parser) {
            log->error("Baseline: rover '{}' parser unavailable; skipping", rover_name);
            continue;
        }

        // Rover a-priori position: config-supplied > RINEX header approx > zero fallback.
        Eigen::Vector3d rov_apriori{ 0.0, 0.0, 0.0 };
        if (rover_cfg.has_position) {
            rov_apriori = Eigen::Vector3d(rover_cfg.position_ecef.data());
        }
        else {
            rov_apriori = rov_parser->getApproxPositionXyz();
        }
        const double apriori_baseline_km = (rov_apriori - ref_pos).norm() / 1000.0;
        log->info("Rover '{}' a-priori ECEF=({:.3f}, {:.3f}, {:.3f}) m, baseline={:.3f} km",
            rover_name, rov_apriori.x(), rov_apriori.y(), rov_apriori.z(),
            apriori_baseline_km);
        if (apriori_baseline_km > bc.max_baseline_km) {
            log->warn("Rover '{}' a-priori baseline ({:.1f} km) exceeds max_baseline_km "
                "({:.1f} km) — short-baseline atmosphere assumption may be invalid",
                rover_name, apriori_baseline_km, bc.max_baseline_km);
        }

        // --- Per-rover Kalman filter resources (slice 5d refactor) ---
        state_manager_ = std::make_unique<StateManager>(3, log_manager_, config_);
        {
            Eigen::VectorXd x0 = state_manager_->getStateVector();
            x0.head<3>() = rov_apriori;
            state_manager_->setStateVector(x0);
            state_manager_->setStateEpochTime(synced.front().time);
        }

        ppp_model_ = std::make_unique<PppModel>(
            config_, *product_parser_, log_manager_, *rov_parser, *state_manager_);
        if (gim_ && gim_->isLoaded()) {
            ppp_model_->setGim(gim_.get());
        }

        // MeasurementModel stores non-owning references to state_manager_
        // and ppp_model_ — must be constructed AFTER both unique_ptrs
        // above point at their final instances.  KalmanFilter likewise
        // captures references; constructed last.
        meas_model_ = std::make_unique<MeasurementModel>(
            *state_manager_, *ppp_model_, config_, log_manager_);
        kalman_filter_ = std::make_unique<KalmanFilter>(
            *state_manager_, config_, log_manager_, *ppp_model_);

        // Per-rover AR.  Each rover gets an independent resolver since
        // its DD ambiguity state set is independent of other rovers'.
        if (config_.getArMode() > 0) {
            AmbiguityResolverConfig ar_config;
            ar_config.ar_mode              = config_.getArMode();
            ar_config.ratio_threshold      = config_.getArRatioThreshold();
            ar_config.elevation_mask_ar    = config_.getArElevationMask();
            ar_config.hold_variance        = config_.getArHoldVariance();
            ar_config.min_epochs           = config_.getArMinEpochs();
            ar_config.max_position_sigma_m = config_.getArMaxPositionSigma();
            ar_config.ratio_upper_bound    = config_.getArRatioUpperBound();
            ar_config.wl_only              = config_.getArWlOnly();
            ar_config.wl_only_pseudo_var   = config_.getArWlOnlyPseudoVar();
            ar_config.wl_max_frac          = config_.getArWlMaxFrac();
            ar_config.min_success_rate     = config_.getArMinSuccessRate();
            ar_config.nl_max_per_pair_frac = config_.getArNlMaxPerPairFrac();
            ar_config.ar_active_systems    = config_.getArActiveSystems();
            ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
            ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
            log->info("Rover '{}' AR enabled: mode={}, wl_only={}, ratio=[{:.1f}, {:.1e}], elev_mask={:.1f} deg, "
                      "hold_var={:.4f}, min_epochs={}, max_pos_σ={:.3f} m",
                rover_name, ar_config.ar_mode, ar_config.wl_only ? "on" : "off",
                ar_config.ratio_threshold, ar_config.ratio_upper_bound,
                ar_config.elevation_mask_ar, ar_config.hold_variance,
                ar_config.min_epochs, ar_config.max_position_sigma_m);
        }
        else {
            ambiguity_resolver_.reset();
            if (rover_idx == 0) {
                log->info("Baseline AR disabled (ar_mode=0); DD ambiguities stay float");
            }
        }

        // Per-rover trace directory.  For multi-rover runs traces land
        // in log_dir_/"rover_<name>" so state_trace.csv,
        // measurement_residuals.csv, etc. don't overwrite each other.
        const std::filesystem::path rover_trace_dir = multi_rover
            ? (log_dir_ / ("rover_" + rover_name))
            : log_dir_;
        if (multi_rover) {
            std::error_code ec;
            std::filesystem::create_directories(rover_trace_dir, ec);
            if (ec) {
                log->warn("Rover '{}' trace dir creation failed: {} ({})",
                    rover_name, rover_trace_dir.string(), ec.message());
            }
        }
        trace_logger_ = std::make_unique<TraceLogger>(
            log_manager_, rover_trace_dir, /*is_ppp_rtk=*/false, config_.getLoggingConfig());
        if (!trace_logger_->isInitialized()) {
            log->warn("Rover '{}' TraceLogger did not initialise; continuing without CSV traces",
                rover_name);
        }

        // Per-rover MW detector — fresh arc state per rover.
        MwSlipDetector mw_rov(config_, log_manager_);

        // Per-rover ambiguity erasure lambda.  Captures state_manager_
        // by reference so each rover sees its own state vector (safe
        // because state_manager_ was just rebuilt above for this rover).
        auto eraseAmbiguitiesForSat = [&](const std::string& sat) -> size_t {
            std::vector<std::string> to_remove;
            for (const auto& [key, idx] : state_manager_->getAmbiguities()) {
                const auto p = StateKeys::parseDdAmbiguityKey(key);
                if (!p) continue;
                if (p->sat_i == sat || p->pivot == sat)
                    to_remove.push_back(key);
            }
            for (const auto& k : to_remove) state_manager_->removeAmbiguity(k);
            return to_remove.size();
            };

        // CSV trace output.  One row per processed epoch.
        std::ofstream trace_ofs;
        const bool emit_trace = config_.getLoggingConfig().enable_state_trace;
        std::filesystem::path trace_path;
        if (emit_trace) {
            trace_path = rover_trace_dir / "baseline_trace.csv";
            trace_ofs.open(trace_path);
            if (trace_ofs) {
                trace_ofs << "week,tow,x,y,z,sigma_x,sigma_y,sigma_z,"
                    "dds_applied,dds_built,n_ambs\n";
            }
            else {
                log->warn("Rover '{}': failed to open baseline trace file: {}",
                    rover_name, trace_path.string());
            }
        }

        // Per-rover counters — declared inside the loop iteration so
        // each rover sees fresh stats and a fresh end-of-rover summary.
        size_t n_with_dd = 0;
        size_t total_dd_obs = 0;
        size_t total_code_dd = 0, total_phase_dd = 0, total_doppler_dd = 0;
        size_t n_geom_ok = 0;
        size_t n_updates = 0;
        std::map<std::string, size_t> pivot_counts;
        std::map<std::string, size_t> constellation_dd;

        // Per-obs-type residual accumulators.  Mixing code (meters),
        // phase (meters but offset by λ·N), and Doppler (Hz) into one
        // aggregate produces nonsense statistics.
        struct ResStats { double sum{ 0.0 }, sq_sum{ 0.0 }, abs_max{ 0.0 }; size_t count{ 0 }; };
        ResStats res_code, res_phase, res_doppler;

        double prev_tow = -1.0;
        int    prev_week = -1;

        size_t n_slips_ref = 0, n_slips_rov = 0, n_amb_resets = 0;

        // Note: the epoch loop body below is intentionally left at the
        // historical 8-space indent rather than being re-indented to
        // 12-space (inside the rover loop).  The diff stays focused on
        // the structural multi-rover wrapping rather than cosmetic
        // re-indentation; a future pass can reflow if desired.
        for (const auto& se : synced) {
            // Tag every log line emitted during this epoch with its
            // RINEX-aligned TOW.  The thread-local context is consumed
            // by the [%T] field in makeGpsFormatter().
            LogManager::setCurrentEpochTime(se.time);

            // --- Station presence + dt =====================================
            // Compute station presence and dt up front so hard-gap detection
            // can run BEFORE the MW slip check (which assumes consecutive-
            // epoch arc continuity).  prev_tow only updates on processed
            // epochs further down, so dt correctly spans any silent interval.
            auto rov_it = se.by_station.find(rover_name);
            auto ref_it = se.by_station.find(bc.reference.name);
            const bool both_present = (rov_it != se.by_station.end()
                && ref_it != se.by_station.end());

            const double dt = (prev_tow < 0.0) ? 0.0
                : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

            // --- Data-gap detection (hard-gap branch) ----------------------
            // When dt exceeds the configured tolerance, neither pre-gap DD
            // ambiguity floats nor pre-gap MW arc statistics are reliable
            // for the post-gap arcs.  Drop them all and let the next epoch
            // re-seed cleanly rather than relying on the per-event recovery
            // path (MW slip detector + phase outlier handler) to mop up.
            // Gated on @c both_present so partial-station epochs don't
            // trigger a reset before we have full data to seed against;
            // prev_tow stays put through the partial epoch and the gap will
            // re-fire on the next full epoch.  Default tolerance of 60 s
            // covers typical 1-Hz / 30-s missions; set
            // processing.data_gap_tolerance_s in the config to override.
            constexpr double kDefaultDataGapToleranceSec = 60.0;
            const double gap_tol = (config_.getDataGapTolerance() > 0.0)
                ? config_.getDataGapTolerance() : kDefaultDataGapToleranceSec;
            if (both_present && dt > gap_tol) {
                log->info("Baseline: hard data gap detected dt={:.3f}s > tolerance={:.1f}s "
                    "(prev_tow={:.3f} -> tow={:.3f}); resetting MW arcs and DD ambiguities",
                    dt, gap_tol, prev_tow, se.time.tow);

                // Erase every DD-keyed ambiguity in the state.  Walk first,
                // remove second, because removeAmbiguity() compacts indices
                // and would invalidate the iteration mid-loop.
                std::vector<std::string> to_drop;
                for (const auto& [key, idx] : state_manager_->getAmbiguities()) {
                    if (key.compare(0, 3, "DD|") == 0) to_drop.push_back(key);
                }
                for (const auto& k : to_drop) state_manager_->removeAmbiguity(k);
                n_amb_resets += to_drop.size();

                // Forget MW running statistics on both receivers and the
                // sticky DD pivots so the next epoch picks fresh ones rather
                // than inheriting the (possibly stale) pre-gap incumbent.
                mw_rov.resetAll();
                mw_ref.resetAll();
                meas_model_->resetBaselinePivots();

                log->info("Baseline: hard-gap reset complete — dropped {} DD ambiguity state(s)",
                    to_drop.size());

                // Layer on the mode-aware position/covariance policy.  The
                // baseline-specific reset above handled DD-keyed state
                // (ambiguities, DD pivots, MW arcs); handleGap covers what
                // it doesn't touch: position cross-covariances and (for
                // kinematic-no-IMU) position reseeding.  Baseline mode has
                // no clock / iono / sat-bias / BSSD-pivot state, so the
                // corresponding helpers inside handleGap are no-ops by
                // construction.  No WLS is run in baseline mode, so we
                // pass an empty WlsResult — handleGap warns and leaves
                // the position VALUE alone, only inflating the variance.
                const bool long_gap = dt > config_.getLongGapTolerance();
                handleGap(dt, long_gap, /*epoch_prefix=*/"", WlsResult{});
            }

            // --- MW pre-filter slip check on both stations -----------------
            // Run before DD build so a slipped satellite's DD ambiguity
            // states are erased before the filter update sees them.  Now
            // safe across hard gaps because the gap reset above has cleared
            // pre-gap arc statistics; MW starts a fresh arc on this epoch
            // and won't false-positive on the post-gap signal levels.
            {
                auto it_ref = se.by_station.find(bc.reference.name);
                auto it_rov = se.by_station.find(rover_name);
                std::set<std::string> slipped;
                if (it_ref != se.by_station.end()) {
                    auto ppp_ref = toMinimalPppObs(it_ref->second, ref_pos,
                                                   ref_ppp_model_->getGeometry(),
                                                   active_set);
                    for (const auto& r : mw_ref.detectLargeMwResets(ppp_ref)) {
                        slipped.insert(r.satellite);
                        ++n_slips_ref;
                    }
                }
                if (it_rov != se.by_station.end()) {
                    const Eigen::Vector3d rov_rx_pos =
                        state_manager_->getStateVector().head<3>();
                    auto ppp_rov = toMinimalPppObs(it_rov->second, rov_rx_pos,
                                                   ppp_model_->getGeometry(),
                                                   active_set);
                    for (const auto& r : mw_rov.detectLargeMwResets(ppp_rov)) {
                        slipped.insert(r.satellite);
                        ++n_slips_rov;
                    }
                }
                if (!slipped.empty()) {
                    size_t removed_total = 0;
                    for (const auto& sat : slipped) {
                        removed_total += eraseAmbiguitiesForSat(sat);
                        // Clear MW arc history at both receivers so the next
                        // epoch starts fresh stats for this sat.
                        mw_ref.resetSatellite(sat);
                        mw_rov.resetSatellite(sat);
                    }
                    n_amb_resets += removed_total;
                    if (removed_total > 0) {
                        std::string sat_list;
                        for (const auto& s : slipped) { sat_list += s; sat_list += ' '; }
                        log->info("MW slip at {:.3f}s: sats={}→ removed {} DD amb state(s)",
                            se.time.tow, sat_list, removed_total);
                    }
                    // If the DD builder's sticky pivot pointed at a slipped
                    // sat, force a re-rank so the next build() picks a clean pivot.
                    meas_model_->resetBaselinePivots();
                }
            }

            // Skip partial-station epochs after MW has had its chance to
            // update one-sided arc statistics.  prev_tow / prev_week are
            // intentionally left untouched so the next full epoch's dt
            // spans the silent interval and triggers gap detection if the
            // outage is long enough.
            if (!both_present) continue;

            // Propagate state by dt before modeled observations are computed,
            // so the rover PppState sees the predicted position.
            if (dt > 0.0) kalman_filter_->predict(dt);
            prev_week = se.time.week;
            prev_tow = se.time.tow;

            // Sync rover and reference PppStates for computeModels().  Rover
            // position comes from StateManager (post-predict); reference is
            // fixed at the configured ECEF.  Clock/tropo/iono are zero —
            // baseline mode doesn't estimate them and they cancel in DD.
            ppp_state_.position = state_manager_->getStateVector().head<3>();
            ppp_state_.clock_bias = 0.0;
            ppp_state_.troposphere_zwd = 0.0;
            ppp_state_.epoch_time = se.time;
            ref_ppp_state_.epoch_time = se.time;

            // Run PppModel on both stations.  Each produces PppObservations
            // with emission-time sat positions, Sagnac, Shapiro, sat-clock,
            // antenna PCO/PCV, solid-earth tide, and per-signal variances
            // from the stochastic model.  residuals = observed - modeled.
            std::vector<PppObservation> rov_modeled, ref_modeled;
            if (!ppp_model_->computeModels(rov_it->second, ppp_state_, rov_modeled) ||
                !ref_ppp_model_->computeModels(ref_it->second, ref_ppp_state_, ref_modeled)) {
                log->debug("Baseline epoch {}: computeModels failed on one or both stations", se.time.tow);
                continue;
            }
            if (rov_modeled.empty() || ref_modeled.empty()) continue;

            ++n_with_dd;
            ++n_geom_ok;

            // Aggregate per-obs-type DD residual diagnostics (using the
            // adapter-built innovations below — filled in after em is built).

            // DD measurement via the unified MeasurementModel overload.
            // Per-epoch rebuild loop: on NeedsMeasurementRebuild the carry
            // set grows with whatever rows the KF outlier handlers flagged,
            // and the next pass re-builds with those rows excluded.  This
            // mirrors the PPP path's rebuild logic at run() ~line 425.  AR
            // and trace logging run only on the final committed pass.  Cap
            // by max_passes so a pathological epoch can't loop forever.
            static bool baseline_first_epoch_log = true;
            if (baseline_first_epoch_log) {
                log->info("Baseline: about to call computeMeasurementUpdate (rov={}, ref={}, state_size={})",
                    rov_modeled.size(), ref_modeled.size(), state_manager_->getStateSize());
                log->flush();
            }

            // Pass budget — shared with the PPP and real-time drivers
            // (computeRebuildPassBudget) so the sizing can't drift; the rover's
            // modeled obs supply the measurement count for the DD baseline.
            const int max_passes =
                computeRebuildPassBudget(*state_manager_, config_, rov_modeled);

            // Constellation lookup used by the post-Applied stats loop.
            auto constellationOfSatPairId = [](const std::string& sid) -> std::string {
                if (sid.empty()) return {};
                switch (sid[0]) {
                case 'G': return "GPS";
                case 'E': return "Galileo";
                case 'R': return "GLONASS";
                case 'C': return "BeiDou";
                case 'J': return "QZSS";
                case 'S': return "SBAS";
                case 'I': return "IRNSS";
                default:  return std::string(1, sid[0]);
                }
                };

            EpochMeasurement em;
            std::set<std::string> carry_excluded;
            size_t built_count = 0;   // rows from the FIRST pass before any KF rejection
            size_t applied = 0;   // rows actually committed to the state (0 if nothing applied)
            bool committed = false;
            bool epoch_failed = false;

            for (int pass = 0; pass < max_passes && !committed; ++pass) {
                try {
                    em = meas_model_->computeMeasurementUpdate(
                        rov_modeled, ref_modeled,
                        state_manager_->getStateVector(),
                        carry_excluded);
                }
                catch (const std::exception& ex) {
                    log->error("Baseline: computeMeasurementUpdate threw on pass {}: {}",
                        pass, ex.what());
                    log->flush();
                    epoch_failed = true;
                    break;
                }
                catch (...) {
                    log->error("Baseline: computeMeasurementUpdate threw unknown exception on pass {}",
                        pass);
                    log->flush();
                    epoch_failed = true;
                    break;
                }
                em.time = se.time;

                if (pass == 0) {
                    built_count = static_cast<size_t>(em.innovations.size());
                    if (baseline_first_epoch_log) {
                        log->info("Baseline first epoch post-DD: em.innov.size={}, em.H=({}x{}), state_size={}",
                            em.innovations.size(), em.H.rows(), em.H.cols(),
                            state_manager_->getStateSize());
                        log->flush();
                        baseline_first_epoch_log = false;
                    }
                }

                if (em.innovations.size() == 0) {
                    if (pass > 0) {
                        log->debug("Baseline: pass {} produced empty measurement after {} exclusions; bailing",
                            pass, carry_excluded.size());
                    }
                    break;
                }
                if (em.H.cols() != static_cast<Eigen::Index>(state_manager_->getStateSize())) {
                    log->warn("Baseline: pass {} H/state size mismatch ({} vs {}); bailing",
                        pass, em.H.cols(), state_manager_->getStateSize());
                    break;
                }

                const auto result = kalman_filter_->update(em);
                if (result == KalmanFilter::UpdateResult::Applied) {
                    applied = static_cast<size_t>(em.innovations.size());
                    ++n_updates;
                    committed = true;
                    if (pass > 0) {
                        log->info("Baseline: applied at pass {} after {} exclusion(s); built={}, applied={}",
                            pass, carry_excluded.size(), built_count, applied);
                    }
                }
                else if (result == KalmanFilter::UpdateResult::NeedsMeasurementRebuild) {
                    carry_excluded = em.excluded_measurements;
                    log->debug("Baseline: KF requested rebuild at pass {} (carry now {} excluded)",
                        pass, carry_excluded.size());
                    // Continue loop — next pass excludes the flagged rows.
                }
                else {  // Skipped
                    log->debug("Baseline: KF skipped epoch at pass {}", pass);
                    break;
                }
            }
            if (epoch_failed) continue;

            if (committed) {
                // --- Post-Applied diagnostics on the surviving rows ---
                // Stats reflect what was actually committed to the state
                // (built - excluded).  Diff between built_count and applied
                // is the row(s) the KF outlier handlers peeled off.
                for (Eigen::Index i = 0; i < em.innovations.size(); ++i) {
                    const double r = em.innovations(i);
                    if (!std::isfinite(r)) continue;
                    const char t = (i < static_cast<Eigen::Index>(em.signal_types.size())
                        && !em.signal_types[static_cast<size_t>(i)].empty())
                        ? em.signal_types[static_cast<size_t>(i)][0] : '?';
                    ResStats* bucket = nullptr;
                    switch (t) {
                    case 'C': bucket = &res_code;    ++total_code_dd;    break;
                    case 'L': bucket = &res_phase;   ++total_phase_dd;   break;
                    case 'D': bucket = &res_doppler; ++total_doppler_dd; break;
                    default: continue;
                    }
                    bucket->sum += r;
                    bucket->sq_sum += r * r;
                    bucket->abs_max = std::max(bucket->abs_max, std::abs(r));
                    ++bucket->count;
                    ++total_dd_obs;

                    if (i < static_cast<Eigen::Index>(em.satellite_ids.size())) {
                        const std::string& sid = em.satellite_ids[static_cast<size_t>(i)];
                        const auto dash = sid.find('-');
                        if (dash != std::string::npos && dash + 1 < sid.size()) {
                            const std::string pivot = sid.substr(dash + 1);
                            const std::string cons = constellationOfSatPairId(pivot);
                            pivot_counts[cons + ":" + pivot]++;
                            constellation_dd[cons]++;
                        }
                    }
                }

                // --- AR (DD ambiguity fixing) on the committed state ---
                // Only DD rows whose four observations all use the same
                // RINEX code are integer-cycle; non-integer-capable rows
                // are excluded from the candidate set.
                if (ambiguity_resolver_) {
                    const auto& exclude_keys =
                        meas_model_->getNonIntegerCapableDdAmbiguityKeys();
                    auto ar_result = ambiguity_resolver_->resolveDd(
                        *state_manager_, rov_modeled, exclude_keys);
                    if (ar_result.success) {
                        state_manager_->setStateVector(ar_result.fixed_state);
                        state_manager_->setCovariance(ar_result.fixed_covariance);
                        ambiguity_resolver_->holdAmbiguitiesDd(*state_manager_, ar_result);
                        log->info("Baseline AR: fixed {}/{} ambs, ratio={:.2f}",
                            ar_result.num_fixed, ar_result.num_candidates,
                            ar_result.ratio);
                    }
                }

                // --- Trace files ---
                if (trace_logger_ && trace_logger_->isInitialized()) {
                    trace_logger_->logState(se.time, *state_manager_);
                    trace_logger_->logResiduals(se.time, em);
                    trace_logger_->logAmbiguities(se.time, *state_manager_);
                }
            }

            // Early-abort diagnostic: if the first 20 epochs with DD observations
            // produce zero filter updates AND zero valid geometry rows, the
            // most common cause is a navigation/orbit file that doesn't cover
            // the observation time window.  Fail loudly rather than running a
            // silent 24-hour loop that produces no solution.
            if (n_with_dd == 20 && n_updates == 0 && n_geom_ok == 0) {
                log->error(
                    "Baseline: after 20 DD epochs, zero filter updates applied and zero valid "
                    "satellite geometry rows.");
                log->error(
                    "  Most common cause: navigation_path covers a different time window than "
                    "the observation files. Product cache loaded {} orbits; check that they "
                    "span week={} tow~{:.0f}.",
                    product_cache.getOrbitCount(), se.time.week, se.time.tow);
                log->error(
                    "  Fix: set navigation_path to a broadcast nav file for the observation day, "
                    "or supply an SP3 file that overlaps the observations.");
                if (trace_ofs) trace_ofs.close();
                return 1;
            }

            if (trace_ofs) {
                const Eigen::Vector3d p = state_manager_->getStateVector().head<3>();
                const Eigen::MatrixXd P = state_manager_->getCovariance();
                const auto sigmaAxis = [&P](Eigen::Index i) {
                    const double v = P(i, i);
                    return (std::isfinite(v) && v >= 0.0) ? std::sqrt(v) : 0.0;
                    };
                trace_ofs << se.time.week << ','
                    << fmt::format("{:.3f}", se.time.tow) << ','
                    << fmt::format("{:.4f}", p.x()) << ','
                    << fmt::format("{:.4f}", p.y()) << ','
                    << fmt::format("{:.4f}", p.z()) << ','
                    << fmt::format("{:.4f}", sigmaAxis(0)) << ','
                    << fmt::format("{:.4f}", sigmaAxis(1)) << ','
                    << fmt::format("{:.4f}", sigmaAxis(2)) << ','
                    << applied << ',' << built_count << ','
                    << state_manager_->getAmbiguities().size() << '\n';
            }
        }

        if (trace_ofs) {
            trace_ofs.close();
            log->info("Baseline position trace written: {} ({} epochs)",
                trace_path.string(), n_updates);
        }

        log->info("DD build summary (rover='{}' vs reference='{}'):",
            rover_name, bc.reference.name);
        log->info("  epochs with DD observations: {}/{}", n_with_dd, synced.size());
        log->info("  epochs with valid geometry:  {}/{}", n_geom_ok, n_with_dd);
        log->info("  total DD scalars: {} (code={}, phase={}, doppler={})",
            total_dd_obs, total_code_dd, total_phase_dd, total_doppler_dd);
        for (const auto& [cons, n] : constellation_dd) {
            log->info("  constellation '{}': {} DD scalars", cons, n);
        }
        std::map<std::string, std::pair<std::string, size_t>> best_pivot;
        for (const auto& [key, n] : pivot_counts) {
            const auto sep = key.find(':');
            if (sep == std::string::npos) continue;
            const std::string cons = key.substr(0, sep);
            const std::string sat = key.substr(sep + 1);
            auto it = best_pivot.find(cons);
            if (it == best_pivot.end() || n > it->second.second) {
                best_pivot[cons] = { sat, n };
            }
        }
        for (const auto& [cons, sat_count] : best_pivot) {
            log->info("  dominant pivot for '{}': {} ({} epochs)",
                      cons, sat_count.first, sat_count.second);
        }
        auto reportBucket = [&](const char* label, const char* unit, const ResStats& rs) {
            if (rs.count == 0) return;
            const double mean = rs.sum / static_cast<double>(rs.count);
            const double rms  = std::sqrt(rs.sq_sum / static_cast<double>(rs.count));
            log->info("  {}-DD residual stats over {} samples: mean={:.3f} {}, rms={:.3f} {}, |max|={:.3f} {}",
                      label, rs.count, mean, unit, rms, unit, rs.abs_max, unit);
        };
        reportBucket("code",    "m",  res_code);
        reportBucket("phase",   "m",  res_phase);
        reportBucket("doppler", "Hz", res_doppler);
        if (res_code.count > 0) {
            log->info("  (short-baseline code-DD expectation: rms of a few tens of cm to ~1 m, "
                      "dominated by multipath + thermal noise)");
            log->info("  (phase residuals contain lambda*N integer offsets before AR; "
                      "interpret phase-DD stats cautiously until Slice 5c-2)");
        }

        // --- Baseline summary (per rover) ---
        const Eigen::Vector3d final_pos = state_manager_->getStateVector().head<3>();
        const Eigen::MatrixXd P_final = state_manager_->getCovariance();
        const Eigen::Vector3d final_sig(
            std::sqrt(std::max(0.0, P_final(0, 0))),
            std::sqrt(std::max(0.0, P_final(1, 1))),
            std::sqrt(std::max(0.0, P_final(2, 2))));
        const double final_baseline_km = (final_pos - ref_pos).norm() / 1000.0;
        const Eigen::Vector3d drift = final_pos - rov_apriori;
        log->info("Baseline solution summary:");
        log->info("  filter updates applied: {}/{}", n_updates, n_with_dd);
        log->info("  final rover ECEF: ({:.4f}, {:.4f}, {:.4f}) m",
                  final_pos.x(), final_pos.y(), final_pos.z());
        log->info("  final sigma (x,y,z): ({:.3f}, {:.3f}, {:.3f}) m",
                  final_sig.x(), final_sig.y(), final_sig.z());
        log->info("  displacement from a-priori: ({:+.4f}, {:+.4f}, {:+.4f}) m (|d|={:.3f} m)",
                  drift.x(), drift.y(), drift.z(), drift.norm());
        log->info("  final baseline length: {:.4f} km", final_baseline_km);
        log->info("  float DD ambiguities held at end: {}", state_manager_->getAmbiguities().size());
        log->info("  MW slip events: reference={}, rover={}, DD-amb resets={}",
                  n_slips_ref, n_slips_rov, n_amb_resets);

        // Roll the per-rover counters into the run-wide totals.  Aggregate
        // summary below uses these to report network throughput.
        total_rover_updates += n_updates;
        total_rover_dd_obs  += total_dd_obs;

    } // end per-rover loop

    // ===================================================================
    // AGGREGATE SUMMARY (multi-rover only adds value here)
    // ===================================================================
    if (multi_rover) {
        log->info("Baseline-Multi aggregate summary:");
        log->info("  rovers processed: {}", bc.rovers.size());
        log->info("  total filter updates across all rovers: {}", total_rover_updates);
        log->info("  total DD scalars across all rovers: {}", total_rover_dd_obs);
        log->info("  per-rover trace files: {}/rover_<name>/baseline_trace.csv",
                  log_dir_.string());
    }
    log->info("Baseline refactor complete (Slice 5d): DD observations now go through the "
              "shared KalmanFilter. Inspect logs/kalman_filter.log and "
              "logs/measurement_residuals.csv for the standard diagnostic surface.");
    if (kalman_filter_) kalman_filter_->logKalmanFilterTimings();
    return 0;
}

} // namespace GnssProcessor
