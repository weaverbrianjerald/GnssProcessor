/**
 * @file ApplicationBaseline.cpp
 * @brief The double-differenced baseline pipeline: Application::runBaseline and its stages.
 *
 * Kept apart from Application.cpp so that each processing mode has its own translation
 * unit.  runBaseline is a short ordered driver (card UOM step U7c) over three levels of
 * named stages, each a block of the former single 1,043-line function moved unchanged
 * (scripts/uom_u7_verify.py proves the move):
 *  - the run: the PathResolver pass, the input checks, the stations' alignment, the
 *    product cache, the reference station's model and MW detector, then one call of
 *    processBaselineRover_ per rover, the no-overlap verdict and the aggregate summary;
 *  - one rover (processBaselineRover_): its overlap with the reference, its a-priori
 *    position, its filter, AR and traces, then one call of processBaselineEpoch_ per
 *    synced epoch, and its summaries;
 *  - one epoch of one rover (processBaselineEpoch_): the duration cutoff, the station
 *    presence and time step, the hard-gap reset, the MW pre-filter, the predict and the
 *    models, the rebuild-pass update loop, the residual statistics with AR and the
 *    traces, the early-abort diagnostic and the position trace row.
 * The values each level shares are the members of BaselineRunWork, BaselineRoverWork and
 * BaselineEpochWork below; a stage opens with an alias block that binds the names its code
 * had as locals of the former function (`auto& n_amb_resets = rover.n_amb_resets;`), so its
 * body reads as it did.  The class is declared in app/Application.hpp.
 */

#include "app/Application.hpp"
#include "app/EpochUpdateSupport.hpp"
#include "filter/StateKeySchema.hpp"
#include "math/TimeUtils.hpp"
#include "model/ModelGeometry.hpp"
#include "products/ProductCache.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/MwSlipDetector.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <fmt/format.h>

namespace GnssProcessor {

// ===========================================================================
// The working values the baseline stages share
// ===========================================================================

/**
 * @brief How one baseline epoch ends for its rover's epoch loop.
 */
enum class BaselineEpochAction {
    Next,       ///< Go on to the next synced epoch (processed or skipped).
    StopRover,  ///< processing.max_duration_s reached: stop this rover's epoch loop.
    AbortRun,   ///< The early-abort diagnostic fired: runBaseline returns 1.
};

/**
 * @brief The values every stage of one baseline run shares.
 *
 * Built by runBaseline once the reference station is set up and before the first rover;
 * it refers to the driver's locals, which outlive it.  The counters roll the rovers' totals
 * into the run's summary.
 */
struct BaselineRunWork {
    const std::shared_ptr<spdlog::logger>& log;  ///< The main_log logger.
    const BaselineConfig& bc;                    ///< The baseline configuration after the PathResolver pass.
    const MultiRinexSync& sync;                  ///< The stations' parsers.
    const std::vector<SyncedEpoch>& synced;      ///< The aligned epochs.
    const Eigen::Vector3d& ref_pos;              ///< The reference ECEF, used as given (the ARP).
    MwSlipDetector& mw_ref;                      ///< The reference's MW detector, reset between rovers.
    const std::set<std::string>& active_set;     ///< The active systems the MW pre-filter keeps.
    const bool multi_rover;                      ///< More than one rover: per-rover trace directories.
    size_t total_rover_updates = 0;              ///< Filter updates over all rovers.
    size_t total_rover_dd_obs  = 0;              ///< DD scalars over all rovers.
    size_t rovers_with_data    = 0;  // rovers that overlap the reference on >=1 epoch
};

/**
 * @brief The values every stage of one rover's processing shares.
 *
 * Built by processBaselineRover_ once the rover's filter, AR, traces and MW detector
 * exist; it refers to that function's locals for the rover's identity and detector.
 */
struct BaselineRoverWork {
    const std::string& rover_name;                 ///< The rover's station name.
    const Eigen::Vector3d& rov_apriori;            ///< Its a-priori ECEF (ARP).
    const std::filesystem::path& rover_trace_dir;  ///< Where its traces go.
    MwSlipDetector& mw_rov;                        ///< Its MW detector (new arcs for each rover).

    std::ofstream trace_ofs;                       ///< baseline_trace.csv, when logging.enable_state_trace.
    std::filesystem::path trace_path;              ///< Its path.


    // Per-rover counters, declared inside the loop iteration so each
    // rover starts with zeroed stats and gets its own end-of-rover summary.
    size_t n_with_dd = 0;
    size_t total_dd_obs = 0;
    size_t total_code_dd = 0, total_phase_dd = 0, total_doppler_dd = 0;
    size_t n_geom_ok = 0;
    size_t n_updates = 0;
    std::map<std::string, size_t> pivot_counts;
    std::map<std::string, size_t> constellation_dd;

    // Per-obs-type residual accumulators.  Code (meters), phase
    // (meters, offset by λ·N) and Doppler (Hz) are kept apart because
    // one aggregate over all three has no meaning.
    struct ResStats { double sum{ 0.0 }, sq_sum{ 0.0 }, abs_max{ 0.0 }; size_t count{ 0 }; };
    ResStats res_code, res_phase, res_doppler;

    double prev_tow = -1.0;
    int    prev_week = -1;

    // processing.max_duration_s cutoff: anchor the data-time window on the
    // first synced epoch and stop once the span reaches the limit (epoch
    // GPS time relative to the first obs).  0 = unlimited.
    double max_dur_anchor_s = 0.0;
    bool   max_dur_anchored = false;

    size_t n_slips_ref = 0, n_slips_rov = 0, n_amb_resets = 0;
};

/**
 * @brief The values the stages of one epoch of one rover share.
 *
 * Built by processBaselineEpoch_ after the duration cutoff and discarded when the epoch
 * ends; the defaults are the initializers the loop's locals had.
 */
struct BaselineEpochWork {
    const SyncedEpoch& se;  ///< The synced epoch.

    // Station presence and the time step (locateBaselineStations_).
    std::map<std::string, std::vector<RinexObservation>>::const_iterator rov_it;  ///< The rover's observations, if present.
    std::map<std::string, std::vector<RinexObservation>>::const_iterator ref_it;  ///< The reference's, if present.
    bool both_present = false;  ///< Both stations are present at this epoch.
    double dt = 0.0;            ///< Seconds since the rover's last processed epoch (0 on the first).

    // The models (modelBaselineEpoch_).
    std::vector<PppObservation> rov_modeled, ref_modeled;

    // The rebuild-pass update loop (runBaselineUpdateLoop_).
    EpochMeasurement em;                     ///< The last pass's DD measurement.
    std::set<std::string> carry_excluded;    ///< Rows the KF outlier handlers flagged, excluded on the next pass.

    size_t built_count = 0;   // rows from the first pass, before any KF rejection
    size_t applied = 0;   // rows actually committed to the state (0 if nothing applied)
    bool committed = false;
    bool epoch_failed = false;
};

namespace {


// Convert a station's epoch RINEX observations into the
// minimum PppObservation subset the MW detector needs.  MW itself
// is a raw code+phase combination and needs no geometry (sat_pos,
// LOS, range, tropo), but @c MwSlipDetector::detectLargeMwResets
// gates each observation by @c obs.elevation against the configured
// elevation mask, so elevation must be populated or every obs is
// silently skipped.  The PppModel's orbit interpolator (the product
// cache orbits) gives the sat position at the receive epoch, and the
// topocentric elevation is computed against the given receiver ECEF
// (the reference's configured position, or the rover's current state
// estimate).  An unresolvable satellite keeps elevation zero and the
// mask drops it.  Populating both @c observed and @c raw_observed
// lets either @c MwFrequencyMode (Mirror or Dual/Triple/All) work.
// Observations from systems not in @c active_systems are dropped here
// so the MW detector never sees them.  Stateless and shared across all
// rovers; the per-rover / per-reference Geometry handle, receiver
// position and active_set are supplied at the call site.
std::vector<PppObservation> toMinimalPppObs(const std::vector<RinexObservation>& rin_obs,
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
        p.lli_flags  = r.lli_flags.toStdMap();
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
}

// Constellation lookup used by the post-Applied stats loop.
std::string constellationOfSatPairId(const std::string& sid) {
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
}

} // namespace

// ---------------------------------------------------------------------------
// runBaseline: the spine
// ---------------------------------------------------------------------------

int Application::runBaseline() {
    auto log = log_manager_.getLogger("main_log");
    const auto& bc_initial = config_.getBaselineConfig();
    const bool is_multi = (config_.getProcessingMode() == ProcessingMode::BaselineMulti);

    log->info("Baseline processing requested (mode={})",
        is_multi ? "Baseline-Multi" : "Baseline-Single");

    if (!resolveBaselineData_(log, bc_initial)) return 1;   // Phase 0: the PathResolver pass

    // Re-read the (possibly updated) baseline config after resolution.
    const auto& bc = config_.getBaselineConfig();
    if (!checkBaselineInputs_(log, bc)) return 1;           // the roster; a RINEX path per station

    // --- Phase 1: multi-station RINEX ingestion + epoch alignment (slice 2) ---
    // Baseline mode is dispatched before loadInputData() (the normal
    // creation site for product_parser_), so construct one here.  Products
    // are loaded below.  DD cancels satellite clocks and biases, so precise
    // clock and bias products are not needed on short baselines, but the
    // satellite positions come from SP3 orbits: ProductParser fills its
    // orbit list only from SP3, and the broadcast navigation file does not
    // supply positions on this path.
    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }
    MultiRinexSync sync(config_, bc, log_manager_, *product_parser_);
    if (!synchronizeBaselineStations_(log, sync)) return 1; // load the stations, align their epochs
    const auto& synced = sync.getSyncedEpochs();
    if (!loadBaselineProducts_(log)) return 1;              // Phase 2: the product cache (SP3 orbits)

    // ===================================================================
    // Shared reference-station setup (once, outside the rover loop)
    //
    // BaselineMulti uses a star topology: each rover is processed
    // independently against the same master reference.  The reference's
    // PppModel state, ECEF, and MW arc detector are constructed once
    // here; their per-rover state (ref_ppp_state_ values, mw_ref running
    // statistics) is reset at the top of each rover iteration so the
    // sequential loop sees a clean reference each time.  The configured
    // reference ECEF is used as given, as the antenna reference point
    // (ARP); no marker-to-ARP reduction is applied to it.
    // ===================================================================
    const RinexParser* ref_parser = sync.parserOf(bc.reference.name);
    if (!ref_parser) {
        log->error("Baseline: reference station parser unavailable");
        return 1;
    }
    const Eigen::Vector3d ref_pos(bc.reference.position_ecef.data());
    buildBaselineReference_(ref_parser, ref_pos, synced);   // its StateManager, PppModel and GIM

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

    log->info("Baseline-Multi orchestrator: {} rover(s) to process against reference '{}'",
              bc.rovers.size(), bc.reference.name);

    // ===================================================================
    // Per-rover loop
    //
    // Sequential processing: each rover gets a newly built filter,
    // measurement model, MW detector, and AR resolver.  Trace files for
    // multi-rover runs go to log_dir_ / "rover_<name>" subdirectories
    // so they do not collide; single-rover runs keep the legacy
    // log_dir_ / "baseline_trace.csv" path for backward compatibility.
    // The shared reference resources above are reset between rovers.
    // ===================================================================
    BaselineRunWork run{ log, bc, sync, synced, ref_pos, mw_ref, active_set, (bc.rovers.size() > 1) };

    for (size_t rover_idx = 0; rover_idx < bc.rovers.size(); ++rover_idx) {
        if (!processBaselineRover_(run, rover_idx)) return 1;  // the early-abort diagnostic fired
    } // end per-rover loop

    // If no rover overlapped the reference on any epoch, the whole run fails
    // (not only a per-rover skip): return nonzero so callers and scripts do
    // not take a run with no solution for success.  Per-rover diagnostics
    // were already logged above.
    if (run.rovers_with_data == 0) {
        log->error("Baseline: no rover overlapped the reference on any synced epoch; "
                   "no solution produced. See per-rover errors above for the cause "
                   "(non-overlapping time windows, sample-rate, or epoch-tag mismatch).");
        return 1;
    }

    reportBaselineAggregate_(run);

    // Optional end-of-run HTML report.  Multi-rover traces live in per-rover
    // subdirs, so generate one report per rover dir; single-rover is flat.
    if (config_.autoReport()) {
        if (trace_logger_) trace_logger_->flush();   // last rover (earlier ones flushed on reconstruction)
        if (run.multi_rover) {
            for (const auto& rv : bc.rovers)
                generateReportFor_(std::filesystem::absolute(log_dir_ / ("rover_" + rv.name)));
        } else {
            generateReportFor_(std::filesystem::absolute(log_dir_));
        }
    }

    return 0;
}

// ===========================================================================
// Setup: the inputs, the stations, the products, the reference station
// ===========================================================================

bool Application::resolveBaselineData_(const std::shared_ptr<spdlog::logger>& log,
                                      const BaselineConfig& bc_initial) {
    // --- Phase 0: PathResolver pass --------------------------------
    // Baseline mode is dispatched before run()'s resolveData() call, so
    // the resolver runs here.  In internal mode it fills in any missing
    // baseline.{reference,rovers}.rinex_path by station-name match under
    // <data root>/<YYYY-DOY>/obs/<station>/.  In production mode it
    // downloads navigation + products; ProductResolver builds the URLs
    // from data_download year/doy, not from the baseline obs files.
    if (config_.isProductionDataSource() && config_.getDownloadConfig().enabled
        && !bc_initial.reference.rinex_path.empty()) {
        // Point the observation_path slot at the reference station, so any
        // date inference that reads observation_path uses the reference
        // station's day.  ProductResolver itself does not read it.
        config_.setObservationPath(bc_initial.reference.rinex_path);
    }
    if (!resolveData()) {
        log->error("Baseline: PathResolver failed");
        return false;
    }
    return true;
}

bool Application::checkBaselineInputs_(const std::shared_ptr<spdlog::logger>& log,
                                      const BaselineConfig& bc) {
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

    // Fail early if PathResolver could not produce a usable set of
    // inputs.  The config loader (loadBaselineSection_) cannot reject an
    // empty rinex_path, because in internal mode PathResolver supplies
    // those values.  Checking here, after resolution, lets the message
    // quote the resolved (or empty) path.
    if (bc.reference.rinex_path.empty()) {
        log->error("Baseline: reference station '{}' has no rinex_path "
                   "after resolution. In internal mode the resolver scans "
                   "data/observations/ for a filename containing '{}' "
                   "(case-insensitive) — make sure such a file exists, "
                   "or set baseline.reference.rinex_path explicitly.",
                   bc.reference.name, bc.reference.name);
        return false;
    }
    for (const auto& rv : bc.rovers) {
        if (rv.rinex_path.empty()) {
            log->error("Baseline: rover '{}' has no rinex_path after resolution. "
                       "Same advice as the reference: drop a matching file under "
                       "data/observations/ or set baseline.rover[s].rinex_path "
                       "explicitly.", rv.name);
            return false;
        }
    }
    if (config_.getNavigationPath().empty()) {
        log->warn("Baseline: no navigation file resolved. "
                  "Internal mode expects data/products/brdc/unzip/*.rnx; "
                  "production mode requires data_download.enabled=true.");
    }
    return true;
}

bool Application::synchronizeBaselineStations_(const std::shared_ptr<spdlog::logger>& log,
                                               MultiRinexSync& sync) {
    if (!sync.load()) {
        log->error("Baseline: failed to load one or more RINEX observation files");
        return false;
    }
    if (!sync.synchronize()) {
        log->error("Baseline: failed to synchronise epochs across stations");
        return false;
    }

    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Baseline: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether station files overlap in time");
        return false;
    }
    log->info("Baseline: multi-station sync complete — {} synchronised epochs ready for DD processing",
              synced.size());
    return true;
}

bool Application::loadBaselineProducts_(const std::shared_ptr<spdlog::logger>& log) {
    // --- Phase 2: Load products + product cache for geometry (slice 5) ---
    // Satellite positions at observation times come from the product
    // cache's orbits, which are SP3 only, so an SP3 file covering the
    // observations is required (getOrbitCount() == 0 fails below).  CLK
    // and BIA products are not needed for short-baseline DD (satellite
    // clocks and biases cancel between receivers), but loading them is
    // harmless if present.
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Baseline: failed to initialise product cache — no broadcast nav / orbits");
        return false;
    }
    log->info("Baseline: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    if (product_cache.getOrbitCount() == 0) {
        log->error("Baseline: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path to a valid RINEX nav file in the config.");
        return false;
    }
    return true;
}

void Application::buildBaselineReference_(const RinexParser* ref_parser,
                                          const Eigen::Vector3d& ref_pos,
                                          const std::vector<SyncedEpoch>& synced) {
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
    // and reference, so the benefit is small on short baselines; the
    // wiring exists for consistency with PPP/PPP-RTK and for long
    // baselines where I₂ no longer cancels.  The same gim_ instance is
    // shared by the reference and per-rover PppModels (set on the rover
    // model where it is constructed inside the rover loop).
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
}

// ===========================================================================
// One rover
// ===========================================================================

bool Application::processBaselineRover_(BaselineRunWork& run, size_t rover_idx) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const auto& sync = run.sync;
    const auto& synced = run.synced;
    const auto& ref_pos = run.ref_pos;
    auto& mw_ref = run.mw_ref;

    const auto& rover_cfg = bc.rovers[rover_idx];
    const std::string& rover_name = rover_cfg.name;

    log->info("=== Baseline rover {}/{}: '{}' ===",
        rover_idx + 1, bc.rovers.size(), rover_name);

    // Reset shared reference state for this rover.  Without this the
    // second rover's reference MW detector would inherit arc statistics
    // from the first rover's run and the ref_ppp_state_ epoch would
    // point at the previous rover's last processed epoch.  (The sticky
    // DD pivots live in meas_model_, which is rebuilt per rover below.)
    // Each rover starts from the same well-defined reference state.
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
        return true;
    }

    if (!roverOverlapsReference_(run, rover_name)) return true;
    const Eigen::Vector3d rov_apriori = baselineRoverApriori_(run, rover_cfg, rov_parser);
    buildBaselineRoverFilter_(rov_parser, rov_apriori, synced);
    buildBaselineRoverAr_(run, rover_idx, rover_name);
    const std::filesystem::path rover_trace_dir = openBaselineRoverTraces_(run, rover_name);

    // Per-rover MW detector: new arc state for each rover.
    MwSlipDetector mw_rov(config_, log_manager_);

    // The rover's working value: its counters, its trace and its previous epoch.
    BaselineRoverWork rover{ rover_name, rov_apriori, rover_trace_dir, mw_rov };
    openBaselineTraceCsv_(run, rover);

    for (const auto& se : synced) {
        const BaselineEpochAction action = processBaselineEpoch_(run, rover, se);
        if (action == BaselineEpochAction::StopRover) break;
        if (action == BaselineEpochAction::AbortRun) return false;
    }

    closeBaselineTraceCsv_(run, rover);
    reportBaselineDdBuild_(run, rover);
    reportBaselineSolution_(run, rover);

    // Roll the per-rover counters into the run-wide totals.  Aggregate
    // summary below uses these to report network throughput.
    run.total_rover_updates += rover.n_updates;
    run.total_rover_dd_obs  += rover.total_dd_obs;
    return true;
}

bool Application::roverOverlapsReference_(BaselineRunWork& run, const std::string& rover_name) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const auto& synced = run.synced;
    auto& rovers_with_data = run.rovers_with_data;

    // Zero-overlap guard: count synced epochs where both this rover and the
    // reference are present.  If zero, the two RINEX files do not overlap
    // within epoch_sync_tolerance_s (disjoint time windows, a sample-rate
    // mismatch, or clock-steered epoch tags).  Without this check the epoch
    // loop runs to completion with both_present always false: n_with_dd
    // stays 0, the 20-epoch early-abort (gated on n_with_dd==20) is never
    // reached, and the run ends with an unexplained "0/0".  The rover is
    // skipped here with an error instead.
    size_t overlap = 0;
    for (const auto& se : synced) {
        if (se.by_station.count(rover_name)
         && se.by_station.count(bc.reference.name)) ++overlap;
    }
    if (overlap == 0) {
        log->error("Baseline: rover '{}' has ZERO epochs overlapping reference "
            "'{}' within epoch_sync_tolerance_s={:.3f}s (of {} synced epochs). "
            "Check that the two RINEX files cover the same time window and "
            "sample rate; nothing to process for this rover.",
            rover_name, bc.reference.name,
            bc.epoch_sync_tolerance_s, synced.size());
        return false;
    }
    ++rovers_with_data;
    log->info("Baseline: rover '{}' overlaps reference on {}/{} synced epochs",
        rover_name, overlap, synced.size());
    return true;
}

Eigen::Vector3d Application::baselineRoverApriori_(const BaselineRunWork& run,
                                                   const StationConfig& rover_cfg,
                                                   const RinexParser* rov_parser) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const auto& ref_pos = run.ref_pos;
    const std::string& rover_name = rover_cfg.name;

    // Rover a-priori position: config-supplied > RINEX header ARP > zero fallback.
    // The header path reduces marker -> ARP (getArpPositionXyz), because the
    // rover position state is antenna-referenced; a config-supplied coordinate is
    // taken at face value (the user owns its reference point).
    Eigen::Vector3d rov_apriori{ 0.0, 0.0, 0.0 };
    if (rover_cfg.has_position) {
        rov_apriori = Eigen::Vector3d(rover_cfg.position_ecef.data());
    }
    else {
        rov_apriori = rov_parser->getArpPositionXyz();
        const Eigen::Vector3d hen = rov_parser->getAntennaDeltaHen();
        if (hen.norm() > 1e-6)
            log->info("Rover '{}' a-priori reduced marker -> ARP via "
                "ANTENNA: DELTA H/E/N ({:.4f}, {:.4f}, {:.4f}) m",
                rover_name, hen(0), hen(1), hen(2));
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
    return rov_apriori;
}

void Application::buildBaselineRoverFilter_(const RinexParser* rov_parser,
                                            const Eigen::Vector3d& rov_apriori,
                                            const std::vector<SyncedEpoch>& synced) {
    // --- Per-rover Kalman filter resources (slice 5d refactor) ---
    state_manager_ = std::make_unique<StateManager>(3, log_manager_, config_);
    state_manager_->logFamiliesAsArmed();   // run-start table "state families as armed"
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
    // and ppp_model_, so it is constructed after both unique_ptrs
    // above point at their final instances.  KalmanFilter also
    // captures references and is constructed last.
    meas_model_ = std::make_unique<MeasurementModel>(
        *state_manager_, *ppp_model_, config_, log_manager_);
    kalman_filter_ = std::make_unique<KalmanFilter>(
        *state_manager_, config_, log_manager_, *ppp_model_);
}

void Application::buildBaselineRoverAr_(const BaselineRunWork& run, size_t rover_idx,
                                        const std::string& rover_name) {
    const auto& log = run.log;

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
        ar_config.wl_only_ffr          = config_.getArWlOnlyFfr();
        ar_config.wl_only_min_arc_count = config_.getArWlOnlyMinArcCount();
        ar_config.wl_only_max_innov_cyc = config_.getArWlOnlyMaxInnovCyc();
        ar_config.nl_max_per_pair_frac = config_.getArNlMaxPerPairFrac();
        ar_config.nl_iono_free_row     = config_.getArNlIonoFreeRow();
        ar_config.enable_partial_ar    = config_.getArEnablePartialAr();
        ar_config.ar_active_systems    = config_.getArActiveSystems();
        ar_config.state_ambiguity_basis = config_.getStateAmbiguityBasis();
        ambiguity_resolver_ = std::make_unique<AmbiguityResolver>(ar_config, log_manager_);
        log->info("Rover '{}' AR enabled: mode={}, wl_only={}, partial_ar={}, ratio=[{:.1f}, {:.1e}], elev_mask={:.1f} deg, "
                  "hold_var={:.4f}, min_epochs={}, max_pos_σ={:.3f} m",
            rover_name, ar_config.ar_mode, ar_config.wl_only ? "on" : "off", ar_config.enable_partial_ar ? "on" : "off",
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
}

std::filesystem::path Application::openBaselineRoverTraces_(const BaselineRunWork& run,
                                                            const std::string& rover_name) {
    const auto& log = run.log;
    const bool multi_rover = run.multi_rover;

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
    // See Application.cpp: uninitialized with zero requested streams is the
    // by-design "no traces armed" case, not a failure.
    if (!trace_logger_->isInitialized() && trace_logger_->requestedStreamCount() > 0) {
        log->warn("Rover '{}' TraceLogger did not initialise; continuing without CSV traces",
            rover_name);
    }
    return rover_trace_dir;
}

// Per-rover ambiguity erasure: every DD ambiguity whose satellite or pivot is
// the slipped satellite.  It reads state_manager_, which processBaselineRover_
// rebuilt for this rover.
std::size_t Application::eraseBaselineAmbiguitiesForSat_(const std::string& sat) {
    std::vector<std::string> to_remove;
    for (const auto& [key, idx] : state_manager_->getAmbiguities()) {
        const auto p = StateKeys::parseDdAmbiguityKey(key);
        if (!p) continue;
        if (p->sat_i == sat || p->pivot == sat)
            to_remove.push_back(key);
    }
    for (const auto& k : to_remove) state_manager_->removeAmbiguity(k);
    return to_remove.size();
}

void Application::openBaselineTraceCsv_(const BaselineRunWork& run, BaselineRoverWork& rover) {
    const auto& log = run.log;
    const auto& rover_name = rover.rover_name;
    const auto& rover_trace_dir = rover.rover_trace_dir;
    auto& trace_ofs = rover.trace_ofs;
    auto& trace_path = rover.trace_path;

    // CSV trace output.  One row per processed epoch.
    const bool emit_trace = config_.getLoggingConfig().enable_state_trace;
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
}

// ===========================================================================
// One epoch of one rover
// ===========================================================================

BaselineEpochAction Application::processBaselineEpoch_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                                       const SyncedEpoch& se) {
    // Tag every log line emitted during this epoch with its
    // RINEX-aligned TOW.  The thread-local context is consumed
    // by the [%T] field in makeGpsFormatter().
    LogManager::setCurrentEpochTime(se.time);
    if (reachedBaselineMaxDuration_(run, rover, se)) return BaselineEpochAction::StopRover;

    // The epoch's working value: station presence, the time step, the models, the update.
    BaselineEpochWork ep{ se };
    locateBaselineStations_(run, rover, ep);   // station presence and the time step
    resetAtBaselineDataGap_(run, rover, ep);   // a hard data gap: DD ambiguities, MW arcs, pivots
    screenBaselineMwSlips_(run, rover, ep);    // the MW pre-filter on both stations

    // Skip partial-station epochs after MW has had its chance to
    // update one-sided arc statistics.  prev_tow / prev_week are
    // intentionally left untouched so the next full epoch's dt
    // spans the silent interval and triggers gap detection if the
    // outage is long enough.
    if (!ep.both_present) return BaselineEpochAction::Next;

    if (!modelBaselineEpoch_(run, rover, ep)) return BaselineEpochAction::Next;
    runBaselineUpdateLoop_(run, rover, ep);
    if (ep.epoch_failed) return BaselineEpochAction::Next;

    recordCommittedBaselineEpoch_(run, rover, ep);   // residual statistics, AR, traces
    if (abortBaselineWithoutUpdates_(run, rover, ep)) return BaselineEpochAction::AbortRun;
    writeBaselineTraceRow_(rover, ep);
    return BaselineEpochAction::Next;
}

bool Application::reachedBaselineMaxDuration_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                              const SyncedEpoch& se) {
    const auto& log = run.log;
    auto& max_dur_anchor_s = rover.max_dur_anchor_s;
    auto& max_dur_anchored = rover.max_dur_anchored;

    // --- Optional max-duration cutoff (processing.max_duration_s) --
    // Anchor on the first synced epoch, then stop once the elapsed
    // data-time span reaches the limit.  Mirrors the PPP / real-time
    // cutoff so all three modes share the same semantics.
    if (config_.getMaxDuration() > 0.0) {
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);
        if (!max_dur_anchored) {
            max_dur_anchor_s = now_s;
            max_dur_anchored = true;
        } else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
            log->info("Baseline: reached max processing duration: {:.1f} s of data "
                      "(limit {:.1f} s); stopping epoch loop",
                      now_s - max_dur_anchor_s, config_.getMaxDuration());
            return true;
        }
    }
    return false;
}

void Application::locateBaselineStations_(const BaselineRunWork& run, const BaselineRoverWork& rover,
                                          BaselineEpochWork& ep) {
    const auto& bc = run.bc;
    const auto& rover_name = rover.rover_name;
    const auto& se = ep.se;
    const auto& prev_tow = rover.prev_tow;
    const auto& prev_week = rover.prev_week;
    auto& rov_it = ep.rov_it;
    auto& ref_it = ep.ref_it;
    auto& both_present = ep.both_present;
    auto& dt = ep.dt;

    // --- Station presence + dt =====================================
    // Compute station presence and dt up front so hard-gap detection
    // can run before the MW slip check (which assumes consecutive-
    // epoch arc continuity).  prev_tow only updates on processed
    // epochs further down, so dt spans any silent interval.
    rov_it = se.by_station.find(rover_name);
    ref_it = se.by_station.find(bc.reference.name);
    both_present = (rov_it != se.by_station.end()
        && ref_it != se.by_station.end());

    dt = (prev_tow < 0.0) ? 0.0
        : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));
}

void Application::resetAtBaselineDataGap_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                          const BaselineEpochWork& ep) {
    const auto& log = run.log;
    auto& mw_ref = run.mw_ref;
    auto& mw_rov = rover.mw_rov;
    auto& n_amb_resets = rover.n_amb_resets;
    const auto& prev_tow = rover.prev_tow;
    const auto& se = ep.se;
    const auto& both_present = ep.both_present;
    const auto& dt = ep.dt;

    // --- Data-gap detection (hard-gap branch) ----------------------
    // When dt exceeds the configured tolerance, neither pre-gap DD
    // ambiguity floats nor pre-gap MW arc statistics are reliable
    // for the post-gap arcs.  Drop them all and let the next epoch
    // re-seed, instead of leaving the cleanup to the per-event
    // recovery path (MW slip detector + phase outlier handler).
    // Gated on @c both_present so a partial-station epoch does not
    // trigger a reset before full data is available to seed against;
    // prev_tow does not advance through the partial epoch, so the gap
    // fires again on the next full epoch.  The default tolerance of
    // 60 s covers typical 1-Hz / 30-s missions; set
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

        // Add the dynamics-mode position/covariance policy.  The
        // baseline-specific reset above handled DD-keyed state
        // (ambiguities, DD pivots, MW arcs); handleGap covers the
        // position: it zeroes position cross-covariances and, on a
        // long static gap or in KinematicNoImu mode, inflates the
        // position variance (KinematicNoImu also resets velocity).
        // Baseline mode has no clock / iono / sat-bias / BSSD-pivot
        // state, so the corresponding helpers inside handleGap are
        // no-ops.  No WLS runs in baseline mode, so an empty
        // WlsResult is passed; in KinematicNoImu mode handleGap then
        // warns and leaves the position value unchanged.
        const bool long_gap = dt > config_.getLongGapTolerance();
        handleGap(dt, long_gap, /*epoch_prefix=*/"", WlsResult{});
    }
}

void Application::screenBaselineMwSlips_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                         const BaselineEpochWork& ep) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const auto& ref_pos = run.ref_pos;
    auto& mw_ref = run.mw_ref;
    const auto& active_set = run.active_set;
    const auto& rover_name = rover.rover_name;
    auto& mw_rov = rover.mw_rov;
    auto& n_slips_ref = rover.n_slips_ref;
    auto& n_slips_rov = rover.n_slips_rov;
    auto& n_amb_resets = rover.n_amb_resets;
    const auto& se = ep.se;

    // --- MW pre-filter slip check on both stations -----------------
    // Run before the DD build so a slipped satellite's DD ambiguity
    // states are erased before the filter update sees them.  Safe
    // across hard gaps: the gap reset above has cleared the pre-gap
    // arc statistics, so MW starts a new arc on this epoch and does
    // not flag the post-gap signal levels as a slip.
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
            removed_total += eraseBaselineAmbiguitiesForSat_(sat);
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

bool Application::modelBaselineEpoch_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                      BaselineEpochWork& ep) {
    const auto& log = run.log;
    auto& prev_week = rover.prev_week;
    auto& prev_tow = rover.prev_tow;
    auto& n_with_dd = rover.n_with_dd;
    auto& n_geom_ok = rover.n_geom_ok;
    const auto& se = ep.se;
    const auto& rov_it = ep.rov_it;
    const auto& ref_it = ep.ref_it;
    const auto& dt = ep.dt;
    auto& rov_modeled = ep.rov_modeled;
    auto& ref_modeled = ep.ref_modeled;

    // Propagate state by dt before modeled observations are computed,
    // so the rover PppState sees the predicted position.
    if (dt > 0.0) kalman_filter_->predict(dt);
    prev_week = se.time.week;
    prev_tow = se.time.tow;

    // Sync rover and reference PppStates for computeModels().  Rover
    // position comes from StateManager (post-predict); reference is
    // fixed at the configured ECEF.  Clock and tropo are zero:
    // baseline mode does not estimate clock/tropo/iono, and they
    // cancel in DD.
    ppp_state_.position = state_manager_->getStateVector().head<3>();
    ppp_state_.clock_bias = 0.0;
    ppp_state_.troposphere_zwd = 0.0;
    ppp_state_.epoch_time = se.time;
    ref_ppp_state_.epoch_time = se.time;

    // Run PppModel on both stations.  Each produces PppObservations
    // with emission-time sat positions, Sagnac, Shapiro, sat-clock,
    // antenna PCO/PCV, solid-earth tide, and per-signal variances
    // from the stochastic model.  residuals = observed - modeled.
    if (!ppp_model_->computeModels(rov_it->second, ppp_state_, rov_modeled) ||
        !ref_ppp_model_->computeModels(ref_it->second, ref_ppp_state_, ref_modeled)) {
        log->debug("Baseline epoch {}: computeModels failed on one or both stations", se.time.tow);
        return false;
    }
    if (rov_modeled.empty() || ref_modeled.empty()) return false;

    ++n_with_dd;
    ++n_geom_ok;
    return true;
}

void Application::runBaselineUpdateLoop_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                         BaselineEpochWork& ep) {
    const auto& log = run.log;
    auto& n_updates = rover.n_updates;
    const auto& se = ep.se;
    const auto& rov_modeled = ep.rov_modeled;
    const auto& ref_modeled = ep.ref_modeled;
    auto& em = ep.em;
    auto& carry_excluded = ep.carry_excluded;
    auto& built_count = ep.built_count;
    auto& applied = ep.applied;
    auto& committed = ep.committed;
    auto& epoch_failed = ep.epoch_failed;

    // Per-obs-type DD residual diagnostics are accumulated after the
    // commit below, from the innovations of the committed pass.

    // DD measurement via the unified MeasurementModel overload.
    // Per-epoch rebuild loop: on NeedsMeasurementRebuild the carry
    // set takes the rows the KF outlier handlers flagged, and the
    // next pass rebuilds with those rows excluded.  This mirrors the
    // PPP path's rebuild loop in Application.cpp.  AR and trace
    // logging run only on the final committed pass.  max_passes caps
    // the loop so a pathological epoch cannot loop forever.
    static bool baseline_first_epoch_log = true;
    if (baseline_first_epoch_log) {
        log->info("Baseline: about to call computeMeasurementUpdate (rov={}, ref={}, state_size={})",
            rov_modeled.size(), ref_modeled.size(), state_manager_->getStateSize());
        log->flush();
    }

    // Pass budget, shared with the PPP and real-time drivers
    // (computeRebuildPassBudget) so the sizing cannot diverge; the
    // rover's modeled obs supply the measurement count for the DD baseline.
    const int max_passes =
        computeRebuildPassBudget(*state_manager_, config_, rov_modeled);

    for (int pass = 0; pass < max_passes && !committed; ++pass) {
        if (!buildBaselineDdPass_(run, ep, pass)) {
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

        // Consume the KF's coherent-pivot-slip request, if any.  The
        // detector keys on the "sat_i-pivot" DD ids and does fire in
        // baseline mode, but there is no SD-PivotManager path here to
        // drain it (forwardKfPivotHandovers is wired only into the PPP /
        // real-time drivers).  If not drained, requested_pivot_handovers_
        // accumulates for the rover's lifetime and mutes the cross-band
        // detectors for good after the first fire.  Drain all three
        // lockstep accumulators and force a DD pivot re-rank on each
        // flagged constellation, excluding the KF's replacement-exclusion
        // sats so the re-rank lands on a clean pivot and existing DD
        // ambiguities are S-transformed (not reset) onto it on the next
        // rebuild pass.
        drainBaselinePivotHandovers_(log);

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
            // Continue the loop; the next pass excludes the flagged rows.
        }
        else {  // Skipped
            log->debug("Baseline: KF skipped epoch at pass {}", pass);
            break;
        }
    }
}

bool Application::buildBaselineDdPass_(const BaselineRunWork& run, BaselineEpochWork& ep, int pass) {
    const auto& log = run.log;
    const auto& rov_modeled = ep.rov_modeled;
    const auto& ref_modeled = ep.ref_modeled;
    const auto& carry_excluded = ep.carry_excluded;
    auto& em = ep.em;

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
        return false;
}
catch (...) {
    log->error("Baseline: computeMeasurementUpdate threw unknown exception on pass {}",
        pass);
    log->flush();
        return false;
}
    return true;
}

void Application::drainBaselinePivotHandovers_(const std::shared_ptr<spdlog::logger>& log) {
    auto ho_cons = kalman_filter_->takeRequestedPivotHandovers();
    if (!ho_cons.empty()) {
        auto ho_excl =
            kalman_filter_->takeRequestedPivotHandoverExcludeSats();
        // Baseline has no iono / sat-bias state; drain to honour
        // the lockstep contract and bound memory, then discard.
        kalman_filter_->takeRequestedPivotHandoverIonoReinflateSats();
        static const std::set<std::string> kNoExcl;
        for (const auto& cons : ho_cons) {
            const auto eit = ho_excl.find(cons);
            const auto& excl =
                (eit != ho_excl.end()) ? eit->second : kNoExcl;
            meas_model_->requestBaselinePivotRerank(cons, excl);
            log->info("Baseline: KF flagged DD pivot slip on '{}'; forcing "
                "pivot re-rank (excluding {} replacement sat(s))",
                cons, excl.size());
        }
    }
}

void Application::recordCommittedBaselineEpoch_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                                const BaselineEpochWork& ep) {
    const auto& log = run.log;
    using ResStats = BaselineRoverWork::ResStats;
    auto& res_code = rover.res_code;
    auto& res_phase = rover.res_phase;
    auto& res_doppler = rover.res_doppler;
    auto& total_code_dd = rover.total_code_dd;
    auto& total_phase_dd = rover.total_phase_dd;
    auto& total_doppler_dd = rover.total_doppler_dd;
    auto& total_dd_obs = rover.total_dd_obs;
    auto& pivot_counts = rover.pivot_counts;
    auto& constellation_dd = rover.constellation_dd;
    const auto& se = ep.se;
    const auto& rov_modeled = ep.rov_modeled;
    const auto& em = ep.em;
    const auto& committed = ep.committed;

    if (!committed) return;
    // --- Post-Applied diagnostics on the surviving rows ---
    // Stats reflect what was committed to the state (built -
    // excluded).  The difference between built_count and applied
    // is the rows the KF outlier handlers removed.
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
        trace_logger_->logReceiverCodeBiases(se.time, *state_manager_);
    }
}

bool Application::abortBaselineWithoutUpdates_(const BaselineRunWork& run, BaselineRoverWork& rover,
                                               const BaselineEpochWork& ep) {
    const auto& log = run.log;
    auto& trace_ofs = rover.trace_ofs;
    const auto& n_with_dd = rover.n_with_dd;
    const auto& n_geom_ok = rover.n_geom_ok;
    const auto& n_updates = rover.n_updates;
    const auto& se = ep.se;
    auto& product_cache = ProductCache::getInstance();

    // Early-abort diagnostic, meant to stop a run whose first 20 epochs
    // with DD observations produce zero filter updates and zero valid
    // geometry rows; the usual cause is an orbit (SP3) product that
    // does not cover the observation window.  It exits with an error
    // instead of running a full day with no solution.  As written the
    // condition cannot hold: n_geom_ok is incremented together with
    // n_with_dd above, so n_geom_ok is 20 whenever n_with_dd is 20.
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
        return true;
    }
    return false;
}

void Application::writeBaselineTraceRow_(BaselineRoverWork& rover, const BaselineEpochWork& ep) {
    auto& trace_ofs = rover.trace_ofs;
    const auto& se = ep.se;
    const auto& built_count = ep.built_count;
    const auto& applied = ep.applied;

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

// ===========================================================================
// The rover's summary and the run's
// ===========================================================================

void Application::closeBaselineTraceCsv_(const BaselineRunWork& run, BaselineRoverWork& rover) {
    const auto& log = run.log;
    auto& trace_ofs = rover.trace_ofs;
    const auto& trace_path = rover.trace_path;
    const auto& n_updates = rover.n_updates;

    if (trace_ofs) {
        trace_ofs.close();
        log->info("Baseline position trace written: {} ({} epochs)",
            trace_path.string(), n_updates);
    }
}

void Application::reportBaselineDdBuild_(const BaselineRunWork& run, const BaselineRoverWork& rover) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const auto& synced = run.synced;
    const auto& rover_name = rover.rover_name;
    const auto& n_with_dd = rover.n_with_dd;
    const auto& n_geom_ok = rover.n_geom_ok;
    const auto& total_dd_obs = rover.total_dd_obs;
    const auto& total_code_dd = rover.total_code_dd;
    const auto& total_phase_dd = rover.total_phase_dd;
    const auto& total_doppler_dd = rover.total_doppler_dd;
    const auto& constellation_dd = rover.constellation_dd;
    const auto& pivot_counts = rover.pivot_counts;
    using ResStats = BaselineRoverWork::ResStats;
    const auto& res_code = rover.res_code;
    const auto& res_phase = rover.res_phase;
    const auto& res_doppler = rover.res_doppler;

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
}

void Application::reportBaselineSolution_(const BaselineRunWork& run, const BaselineRoverWork& rover) {
    const auto& log = run.log;
    const auto& ref_pos = run.ref_pos;
    const auto& rov_apriori = rover.rov_apriori;
    const auto& n_updates = rover.n_updates;
    const auto& n_with_dd = rover.n_with_dd;
    const auto& n_slips_ref = rover.n_slips_ref;
    const auto& n_slips_rov = rover.n_slips_rov;
    const auto& n_amb_resets = rover.n_amb_resets;

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
}

void Application::reportBaselineAggregate_(const BaselineRunWork& run) {
    const auto& log = run.log;
    const auto& bc = run.bc;
    const bool multi_rover = run.multi_rover;
    const auto& total_rover_updates = run.total_rover_updates;
    const auto& total_rover_dd_obs = run.total_rover_dd_obs;

    // ===================================================================
    // Aggregate summary (multi-rover runs only)
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
}

} // namespace GnssProcessor
