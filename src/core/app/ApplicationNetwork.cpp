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
#include "products/NetworkProductIO.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// Per-station runtime bundle: an independent single-receiver StateManager /
/// PppModel / MeasurementModel (single-receiver invariants intact — the network
/// couples stations only through the shared satellite border, never by aliasing
/// their core indices).  Coordinates are held fixed at @c ecef.
struct NetStation {
    std::string                       name;
    Eigen::Vector3d                   ecef{Eigen::Vector3d::Zero()};
    bool                              is_pivot{false};
    std::unique_ptr<StateManager>     sm;
    std::unique_ptr<PppModel>         model;
    std::unique_ptr<MeasurementModel> mm;
    PppState                          pstate;
};

/// One shared parameter carried across epochs (D2): its value and posterior
/// variance, re-keyed by (kind, sat, signal) identity so it survives layout
/// changes (a satellite blinking in/out).
struct CarryPrior {
    SharedParamKind kind;
    std::string     sat;
    std::string     signal;
    double          value;
    double          var;
};

} // namespace

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

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

    // --- Phase 1: resolve products + broadcast nav --------------------------
    // Station RINEX is explicit, so resolveData() here only fills product /
    // navigation paths (orbits + clocks needed for per-station geometry).  A
    // failure is non-fatal: the product-cache load below surfaces a precise
    // "no orbits" diagnostic if products are genuinely missing.
    if (!resolveData()) {
        log->warn("Network: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config (product-cache load will report "
                  "if orbits are missing).");
    }

    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }

    // --- Phase 2: multi-station RINEX ingestion + epoch alignment -----------
    // MultiRinexSync is driven by a BaselineConfig roster; adapt the network
    // stations into it with the clock-datum station as the "reference" so the
    // synced schedule walks its epochs (by_station includes the reference).
    BaselineConfig roster;
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
    const bool closed_loop_on = cl.enable && !cl.station.name.empty();
    if (closed_loop_on) {
        roster.rovers.push_back(cl.station);
        log->info("Network: closed-loop consumer '{}' added to the sync roster "
                  "(held OUT of the joint solve)", cl.station.name);
    }

    MultiRinexSync sync(config_, roster, log_manager_, *product_parser_);

    // Clip each station's parse to the processing window.  The epoch loop below
    // is bounded by processing.max_duration_s (anchored on the first synced
    // epoch); without this, load() would parse — and synchronize() would copy —
    // the FULL 24h of every station even when only the first minutes are used,
    // which dominates wall-time at ~50 full-day stations.  A small margin covers
    // per-station first-epoch jitter; max_duration_s <= 0 keeps full-file parsing.
    if (config_.getMaxDuration() > 0.0) {
        const double span_s = config_.getMaxDuration() + 60.0;
        sync.setParseWindowSeconds(span_s);
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
    if (product_cache.getOrbitCount() == 0) {
        log->error("Network: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path / products to cover the observation day.");
        return 1;
    }

    // --- Phase 4: per-station Kalman resources (pivot at index 0) -----------
    // Each station: a single-receiver StateManager (coords fixed at the
    // configured ECEF), PppModel bound to its RinexParser, and a MeasurementModel.
    // Pivot first so PASS A can seed its phase-signal set before the others.
    std::vector<NetStation> stations;
    auto buildStation = [&](const StationConfig& cfg, bool is_pivot) -> bool {
        const RinexParser* parser = sync.parserOf(cfg.name);
        if (!parser) {
            log->error("Network: station '{}' parser unavailable", cfg.name);
            return false;
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
        st.mm = std::make_unique<MeasurementModel>(
            *st.sm, *st.model, config_, log_manager_);
        st.pstate.position        = st.ecef;
        st.pstate.clock_bias      = 0.0;
        st.pstate.troposphere_zwd = 0.0;
        stations.push_back(std::move(st));
        return true;
    };
    if (!buildStation(roster.reference, /*is_pivot=*/true)) {
        log->error("Network: clock datum station '{}' failed to load; the network "
                   "cannot be built without its datum. Aborting.",
                   roster.reference.name);
        return 1;
    }
    for (const auto& rv : roster.rovers) {
        if (closed_loop_on && rv.name == cl.station.name) continue;  // held-out consumer
        if (!buildStation(rv, /*is_pivot=*/false)) {
            log->warn("Network: station '{}' failed to load; skipping it (the "
                      "network proceeds with the remaining stations).", rv.name);
            continue;
        }
    }
    log->info("Network: {} station estimators constructed (pivot '{}')",
              stations.size(), stations.front().name);

    // --- Owned product sink (closed-loop isolation) -------------------------
    SsrCorrectionStore network_store;
    NetworkProductEmitter emitter;

    // --- Optional on-disk product serialization -----------------------------
    // A reusable per-epoch dump of the emitted solution (the same pre-emit
    // NetworkSolutionEpoch the in-memory closed loop consumes, plus this epoch's
    // datum-change flag).  Lets consumer-side experiments — notably the consumer
    // AR-frame work — replay the products WITHOUT re-running the multi-station
    // network: NetworkProductReader::replay reproduces this store byte-for-byte
    // (discontinuity counters included).
    std::unique_ptr<NetworkProductWriter> product_writer;
    if (!nc.product_output_path.empty()) {
        // A relative path lands under the network trace dir, alongside
        // network_solution.csv etc.; an absolute path is used verbatim.
        std::filesystem::path prod_path = nc.product_output_path;
        if (prod_path.is_relative()) prod_path = log_dir_ / prod_path;
        product_writer = std::make_unique<NetworkProductWriter>(prod_path.string());
        if (product_writer->good()) {
            log->info("Network: serializing per-epoch products to '{}'", prod_path.string());
        } else {
            log->error("Network: failed to open product_output_path '{}' for writing; "
                       "disabling on-disk serialization for this run.", prod_path.string());
            product_writer.reset();
        }
    }

    // --- Closed-loop consumer (held-out station) ----------------------------
    // The held-out station runs the user-side PPP-RTK + AR pipeline through a
    // RealTimeEngine fed epoch-by-epoch from RINEX replay, with its SSR
    // consumption repointed at the OWNED network_store (never the singleton).
    // Each epoch: the producer emits into network_store, then the consumer reads
    // that fresh epoch — closing the loop "does our own user engine recover
    // integers from our own products on an independent station?".
    GnssConfig consumer_config = config_.deriveClosedLoopConsumerConfig();
    std::unique_ptr<RealTimeEngine> consumer;
    if (closed_loop_on) {
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
                          "once the float σ converges below the gate ({:.2f} m).",
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
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — reading the OWNED network store ({}).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off",
                          cl.consume_network_clock
                              ? "full: network clock + phase bias"
                              : "bias-only: precise clock + network phase bias");
            } else {
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — BASELINE mode (precise products only, network "
                          "store NOT injected).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off");
            }
        }
    }

    // Closed-loop metrics + trace.
    const Eigen::Vector3d cl_truth(cl.station.position_ecef.data());
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    std::size_t cl_epochs = 0, cl_ar_ran = 0, cl_ar_fixed = 0;
    long long   cl_sum_fixed = 0;
    double cl_first_fix_tow = -1.0; int cl_first_fix_week = -1;
    std::deque<double> cl_recent_err3d;   // last-N 3D errors → converged-segment RMS
    constexpr std::size_t kClRecentWindow = 30;
    std::ofstream cl_trace;

    // D2 process noise (variance growth per second) for the carry-forward prior.
    const double q_clock = config_.getProcessNoise().sat_clock;        // m²/s
    const double q_pb    = config_.getProcessNoise().sat_phase_bias;   // m²/s

    // --- Trace --------------------------------------------------------------
    std::ofstream trace;
    const std::filesystem::path trace_path = log_dir_ / "network_trace.csv";
    // Per-satellite product trace: the actual emitted solution (long format, one
    // row per shared parameter per epoch) so the producer's output is inspectable
    // — per-sat clock-Δ continuity, phase-bias stability, and discontinuity bumps.
    std::ofstream soln;
    const std::filesystem::path soln_path = log_dir_ / "network_solution.csv";
    {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        trace.open(trace_path);
        if (trace) {
            trace << "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,"
                     "gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins\n";
        } else {
            log->warn("Network: failed to open trace file {}", trace_path.string());
        }
        soln.open(soln_path);
        if (soln) soln << "week,tow,sat,kind,signal,value_m,disc_counter\n";
        else log->warn("Network: failed to open solution trace {}", soln_path.string());

        if (closed_loop_on && consumer) {
            const std::filesystem::path cl_path = log_dir_ / "closed_loop_trace.csv";
            cl_trace.open(cl_path);
            if (cl_trace)
                cl_trace << "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
                            "ratio,pos_err_3d_m,east_m,north_m,up_m\n";
            else
                log->warn("Network: failed to open closed-loop trace {}", cl_path.string());
        }
    }

    // === Epoch loop =========================================================
    std::vector<CarryPrior> prev_carry;                 // D2 stash from the last solve
    std::map<std::string, std::string> prev_pivot;      // constellation -> pivot sat (handover detect)
    double prev_tow = -1.0;
    int    prev_week = -1;
    int    iod = 0;
    size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0;
    double max_dur_anchor_s = 0.0;
    bool   max_dur_anchored = false;
    bool   spectrum_diag_done = false;  // one-shot estimability spectrum dump

    // Column-split helper: split a station's H into the shared border A_s
    // (clock/phase-bias columns) and the local nuisance block A_r (everything
    // except the 3 fixed-position columns and the shared columns).  Returns the
    // H-column → A_r-column map (used for the pivot ambiguity pins).
    auto splitStation = [&](NetStation& st, const NetworkEstimator& est,
                            const EpochMeasurement& em,
                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                            Eigen::VectorXd& w, Eigen::VectorXd& y,
                            std::map<int, int>& h2local) {
        const Eigen::Index m       = em.H.rows();
        const Eigen::Index nstate  = em.H.cols();
        const Eigen::Index nshared = est.numShared();
        A_s = Eigen::MatrixXd::Zero(m, nshared);

        std::set<int> shared_cols;  // H-column indices that map to the shared border
        for (const auto& kv : st.sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }
        for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(kv.first);
            if (!parts) continue;
            Eigen::Index col;
            try { col = est.phaseBiasColumn(parts->satellite, parts->signal); }
            catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }

        const int pos = static_cast<int>(st.sm->getPositionIndex());  // 0
        std::vector<int> local_cols;
        local_cols.reserve(static_cast<size_t>(nstate));
        for (int c = 0; c < static_cast<int>(nstate); ++c) {
            if (c == pos || c == pos + 1 || c == pos + 2) continue;  // coords fixed
            if (shared_cols.count(c)) continue;                      // shared border
            h2local[c] = static_cast<int>(local_cols.size());
            local_cols.push_back(c);
        }
        A_r.resize(m, static_cast<Eigen::Index>(local_cols.size()));
        for (size_t j = 0; j < local_cols.size(); ++j)
            A_r.col(static_cast<Eigen::Index>(j)) = em.H.col(local_cols[j]);

        w = em.R_v.cwiseInverse();   // per-obs information = 1/σ²
        y = em.innovations;
    };

    for (const auto& se : synced) {
        LogManager::setCurrentEpochTime(se.time);

        // Optional max-duration cutoff (processing.max_duration_s): anchor on the
        // first synced epoch, stop once the elapsed data span reaches the limit.
        if (config_.getMaxDuration() > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
            else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
                log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                          config_.getMaxDuration());
                break;
            }
        }

        const double dt = (prev_tow < 0.0) ? 0.0
            : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

        // --- PASS A: per-station geometry + shared-state layout -------------
        std::vector<std::vector<PppObservation>> modeled(stations.size());
        std::vector<bool> present(stations.size(), false);

        // Pivot first (its usable phase signals restrict the shared phase-bias
        // columns to ones with a pivot-receiver ambiguity pin).
        {
            NetStation& piv = stations.front();
            auto it = se.by_station.find(piv.name);
            if (it == se.by_station.end() || it->second.empty()) {
                // No datum station this epoch: no clean phase-bias gauge — skip.
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                continue;
            }
            piv.pstate.epoch_time = se.time;
            if (!piv.model->computeModels(it->second, piv.pstate, modeled.front())
                || modeled.front().empty()) {
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                continue;
            }
            piv.mm->ensureNetworkSharedStates(modeled.front());  // allow all pivot phase signals
            present.front() = true;
        }

        // The pivot's allocated phase-bias keys define the shared phase-bias set.
        std::set<std::string> shared_phase_keys;
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);

        for (size_t i = 1; i < stations.size(); ++i) {
            NetStation& st = stations[i];
            auto it = se.by_station.find(st.name);
            if (it == se.by_station.end() || it->second.empty()) continue;
            st.pstate.epoch_time = se.time;
            if (!st.model->computeModels(it->second, st.pstate, modeled[i])
                || modeled[i].empty())
                continue;
            st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys);
            present[i] = true;
        }

        // --- Cross-station observability pre-filter -------------------------
        // A satellite clock is only well-determined for a network product when at
        // least kMinStationsPerSat stations observe it: a single-station satellite
        // clock is tied to just that one receiver clock and, at marginal geometry,
        // becomes the extra near-zero direction that trips the rank gate.  Drop
        // such satellites from the network ENTIRELY this epoch — both the shared
        // clock/phase-bias states AND the underlying observations.  Excluding the
        // observations is essential: a satellite kept in the design with NO shared
        // clock column would pin its station's receiver clock absolutely and so
        // erase the very clock gauge the datum is meant to pin (declared > observed
        // → the gate would then reject every epoch).  Dropping the whole satellite
        // lets one weak satellite no longer force the WHOLE epoch to be withheld;
        // the gate stays as the final safety net for a genuine residual deficiency.
        constexpr int kMinStationsPerSat = 2;
        int n_dropped_sats = 0;
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
        // The pivot's pruned phase-bias states define the final shared phase-bias
        // set (every column then has a backing pivot-receiver ambiguity pin).
        shared_phase_keys.clear();
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);

        // --- Register the shared column layout (fresh estimator per epoch) ---
        NetworkEstimator est;
        std::vector<std::string> registered_clock_sats;
        std::set<std::string> clk_seen;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates()) {
                const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
                est.registerClock(sat);
                if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
            }
        }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (parts) est.registerPhaseBias(parts->satellite, parts->signal);
        }

        const int n_clock = static_cast<int>(registered_clock_sats.size());
        const int n_pb    = static_cast<int>(shared_phase_keys.size());
        const int n_present = static_cast<int>(std::count(present.begin(), present.end(), true));

        if (est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ",0,0,0,0,0,0,0,0\n";
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        est.beginEpoch();

        // --- PASS B: per-station assembly + add to the arrowhead ------------
        int total_pins = 0;
        bool epoch_ok = true;
        for (size_t i = 0; i < stations.size() && epoch_ok; ++i) {
            if (!present[i]) continue;
            NetStation& st = stations[i];

            EpochMeasurement em = st.mm->computeMeasurementUpdate(
                modeled[i], st.sm->getStateVector());
            if (em.innovations.size() == 0) { present[i] = false; continue; }
            if (em.H.cols() != static_cast<Eigen::Index>(st.sm->getStateSize())) {
                log->warn("Network: station '{}' H/state mismatch ({} vs {})",
                          st.name, em.H.cols(), st.sm->getStateSize());
                if (st.is_pivot) { epoch_ok = false; break; }  // no datum without the pivot
                present[i] = false;
                continue;
            }

            Eigen::MatrixXd A_s, A_r;
            Eigen::VectorXd w, y;
            std::map<int, int> h2local;
            splitStation(st, est, em, A_s, A_r, w, y, h2local);

            // Pivot folds tight-prior pins on its own ambiguities (the LOCAL
            // half of the S-basis): drive each pinned ambiguity's ABSOLUTE value
            // to 0, so the estimable phase bias absorbs the pivot-receiver lump.
            if (st.is_pivot) {
                const Eigen::VectorXd piv_x = st.sm->getStateVector();
                std::vector<int> pin_col;       // A_r column of each pinned ambiguity
                std::vector<double> pin_y;       // innovation = -current absolute value
                for (const auto& pk : shared_phase_keys) {
                    const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                    if (!parts) continue;
                    const std::string amb_key = parts->satellite + "_" + parts->signal;
                    const int amb_idx = st.sm->getAmbiguityIndex(amb_key);
                    if (amb_idx < 0) continue;  // pivot has no usable phase row for it this epoch
                    const auto hit = h2local.find(amb_idx);
                    if (hit == h2local.end()) continue;
                    pin_col.push_back(hit->second);
                    pin_y.push_back(-piv_x(amb_idx));
                }
                const int np = static_cast<int>(pin_col.size());
                if (np > 0) {
                    const Eigen::Index m0 = A_s.rows();
                    A_s.conservativeResize(m0 + np, Eigen::NoChange);
                    A_s.bottomRows(np).setZero();
                    A_r.conservativeResize(m0 + np, Eigen::NoChange);
                    A_r.bottomRows(np).setZero();
                    w.conservativeResize(m0 + np);
                    y.conservativeResize(m0 + np);
                    for (int k = 0; k < np; ++k) {
                        A_r(m0 + k, pin_col[k]) = 1.0;
                        w(m0 + k) = 1e10;   // tight prior (exact gauge)
                        y(m0 + k) = pin_y[k];
                    }
                    total_pins += np;
                }
            }

            try {
                est.addStation(A_s, A_r, w, y);
            } catch (const std::exception& ex) {
                // One station's rank-deficient / ill-conditioned local block must
                // not sink the whole epoch in a large heterogeneous network: drop
                // it and keep the rest (the null-space gate still guards the
                // result).  The PIVOT is the exception — it carries the S-basis, so
                // without it the epoch has no datum and must be skipped.
                if (st.is_pivot) {
                    log->warn("Network: PIVOT '{}' addStation rejected ({}); skipping epoch",
                              st.name, ex.what());
                    epoch_ok = false;
                    break;
                }
                log->warn("Network: station '{}' addStation rejected ({}); dropping it this epoch",
                          st.name, ex.what());
                present[i] = false;
                continue;
            }
        }

        if (!epoch_ok || est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << 0 << ",0,0,0,0," << total_pins << '\n';
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // --- Datum + null-space gate + solve --------------------------------
        NetworkSbasis sb;
        SbasisDeclaration decl;
        decl.reference_receiver_id = nc.clock_datum_station;
        if (!nc.clock_datum_satellite.empty()) {
            decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                = nc.clock_datum_satellite;
        }

        NetworkSbasis::Plan plan;
        try {
            plan = sb.buildSharedDatum(est, registered_clock_sats, decl);
        } catch (const std::exception& ex) {
            log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
            ++n_skipped;
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Pivot handover: the previous epoch's shared estimate lives in the OLD
        // datum, so a changed clock pivot invalidates the D2 carry-forward (and
        // forces a discontinuity bump downstream).
        bool datum_changed = false;
        for (const auto& kv : plan.clock_pivot) {
            const auto it = prev_pivot.find(kv.first);
            if (it != prev_pivot.end() && it->second != kv.second) datum_changed = true;
        }

        // Null-space rank gate.  The reduced shared normals are deliberately
        // ill-conditioned: the phase rows pin the satellite-clock / phase-bias
        // SUM to ~σ_phase precision (weight ~1e5), while CODE alone separates the
        // two (the clock-vs-bias split), so that split is orders of magnitude
        // weaker — yet still firmly non-zero.  Empirically the spectrum shows a
        // clean ~7-decade gap between the true gauge freedoms (the per-
        // constellation clock datum, σ ~ 1e-10 of σ_max) and the weakest REAL
        // direction (σ ~ 1e-2).  The default 1e-6 relative tolerance sits above
        // the weak-but-real directions and miscounts them as deficiencies; a
        // 1e-9 relative tolerance lands squarely in the gap, counting only the
        // genuine gauges while still catching a truly unpinned datum.
        constexpr double kNullSpaceTol = 1e-9;

        // ---- One-shot estimability diagnostic -----------------------------
        // Dump the tail of the reduced shared-normal singular spectrum for the
        // first epoch that reaches the gate, so we can see WHERE the null space
        // sits relative to the declared datum dimension: a clean ~7-decade gap
        // at exactly declared_dim ⇒ the 1e-9 tolerance is mis-set; one or more
        // EXTRA tiny singular values above the gap ⇒ a genuinely unpinned
        // direction (a missing datum constraint).
        if (!spectrum_diag_done) {
            spectrum_diag_done = true;
            const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
            Eigen::JacobiSVD<Eigen::MatrixXd> svd(Nss);
            const Eigen::VectorXd& sv = svd.singularValues();
            const Eigen::Index k = sv.size();
            const double smax = (k > 0) ? sv(0) : 0.0;
            const double cutoff = kNullSpaceTol * std::max(smax, 1.0);
            int below = 0;
            for (Eigen::Index i = 0; i < k; ++i) if (sv(i) < cutoff) ++below;
            const Eigen::Index ntail = std::min<Eigen::Index>(14, k);
            std::string tail;
            for (Eigen::Index i = k - ntail; i < k; ++i)
                tail += fmt::format("{}{:.3e}", (i == k - ntail ? "" : ", "), sv(i));
            log->warn("Network[estimability]: TOW {:.1f} reduced_N_ss {}x{}, "
                      "declared_dim={}, sv_max={:.3e}, cutoff={:.3e}, {} sv<cutoff; "
                      "smallest {} sv = [{}]",
                      se.time.tow, Nss.rows(), Nss.cols(), plan.declared_dimension,
                      smax, cutoff, below, ntail, tail);
        }

        bool gate_ok = true;
        try {
            sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                                   kNullSpaceTol);
        } catch (const std::exception& ex) {
            // The reduced shared system carries MORE gauge freedoms than the
            // declared datum can pin — typically a satellite that is momentarily
            // unobservable (seen by a single station at marginal geometry).  An
            // under-determined product would be a meaningless least-norm split,
            // so this epoch's product is WITHHELD (the plan's "gate emission
            // until convergence" observability safeguard).  Per-satellite
            // observability gating (drop the weak satellite, emit the rest) is a
            // later network-QC slice.
            gate_ok = false;
            ++n_gate_withheld;
            log->debug("Network: observability gate at TOW {:.1f} withheld product ({})",
                       se.time.tow, ex.what());
        }
        if (!gate_ok) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",0,0,0,0," << total_pins << '\n';
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // --- D2 carry-forward prior (diagonal, inflated over the gap) -------
        // Applied AFTER the gate (which must see the DATA-only gauge structure)
        // and BEFORE the solve.  The prior anchors each non-datum shared
        // parameter to its previous-epoch estimate; it is skipped on a pivot
        // handover, when the carried values belong to a now-retired datum.
        if (!datum_changed && !prev_carry.empty()) {
            std::vector<NetworkEstimator::SharedPrior> priors;
            priors.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                const double q = (cp.kind == SharedParamKind::Clock) ? q_clock : q_pb;
                const double var_eff = cp.var + q * std::max(0.0, dt);
                if (!(var_eff > 0.0)) continue;
                priors.push_back(NetworkEstimator::SharedPrior{
                    cp.kind, cp.sat, cp.signal, cp.value, 1.0 / var_eff});
            }
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
        }

        if (datum_changed) emitter.markDatumChange();
        prev_pivot = plan.clock_pivot;

        Eigen::VectorXd x_s;
        try {
            x_s = est.solve(plan.shared_datum);
        } catch (const std::exception& ex) {
            log->warn("Network: solve failed at TOW {:.1f} ({}); skipping epoch",
                      se.time.tow, ex.what());
            ++n_skipped;
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // --- Emit -----------------------------------------------------------
        const NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++);
        emitter.emitToStore(sol, network_store);
        if (product_writer) product_writer->write(sol, datum_changed);
        ++n_emitted;

        // --- Closed-loop consume: held-out station vs the fresh network store -
        // Run the consumer ONLY now that this epoch's products are in the store,
        // so its SSR clock + phase bias reflect the just-solved network solution.
        if (closed_loop_on && consumer) {
            auto cit = se.by_station.find(cl.station.name);
            if (cit != se.by_station.end() && !cit->second.empty()) {
                consumer->processEpoch(cit->second);
                ++cl_epochs;

                const Eigen::Vector3d pos = consumer->getPosition();
                const Eigen::Vector3d d   = pos - cl_truth;
                const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, cl_truth_llh);
                const double err3d = d.norm();

                const bool ar_ran = consumer->lastArRan();
                const AmbiguityResolution& ar = consumer->lastArResult();
                const bool fixed = ar_ran && ar.success;
                if (ar_ran) ++cl_ar_ran;
                if (fixed) {
                    ++cl_ar_fixed;
                    cl_sum_fixed += ar.num_fixed;
                    if (cl_first_fix_tow < 0.0) {
                        cl_first_fix_tow = se.time.tow; cl_first_fix_week = se.time.week;
                    }
                }
                // Converged-segment position RMS: rolling window over the most
                // recent epochs (the float transient + any early wrong-fix
                // excursions are not a fair measure of steady-state quality).
                cl_recent_err3d.push_back(err3d);
                if (cl_recent_err3d.size() > kClRecentWindow) cl_recent_err3d.pop_front();

                if (cl_trace)
                    cl_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                             << cit->second.size() << ',' << (ar_ran ? 1 : 0) << ','
                             << (fixed ? 1 : 0) << ',' << (ar_ran ? ar.num_fixed : 0) << ','
                             << (ar_ran ? ar.num_candidates : 0) << ','
                             << fmt::format("{:.2f}", ar_ran ? ar.ratio : 0.0) << ','
                             << fmt::format("{:.4f}", err3d) << ','
                             << fmt::format("{:.4f}", enu.x()) << ','
                             << fmt::format("{:.4f}", enu.y()) << ','
                             << fmt::format("{:.4f}", enu.z()) << '\n';
            }
        }

        // Per-satellite product trace (after emit, so discontinuity counters are current).
        if (soln) {
            for (const auto& s : sol.sats) {
                if (s.has_clock)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",CLK,," << fmt::format("{:.4f}", s.clock_c0_m) << ",0\n";
                for (const auto& [signal, pb] : s.phase_bias)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",PB," << signal << ',' << fmt::format("{:.4f}", pb.bias_m)
                         << ',' << emitter.discontinuityCounter(s.sat, signal) << '\n';
            }
        }

        // --- D2 stash for the next epoch ------------------------------------
        prev_carry.clear();
        const Eigen::MatrixXd& cov = est.sharedCovariance();
        std::set<Eigen::Index> pinned_cols;
        for (const auto& p : plan.shared_datum) pinned_cols.insert(p.column);
        auto stash = [&](SharedParamKind kind, const std::string& sat,
                         const std::string& signal, Eigen::Index col) {
            // Skip the datum-pinned columns.  Their solve covariance is 1/pin_scale
            // (not ~0), but the explicit datum re-pins them every epoch (its
            // Dirichlet elimination overrides any prior on them), and carrying one
            // forward would wrongly anchor a retired pivot to its old datum value
            // after a handover.
            if (pinned_cols.count(col)) return;
            const double var = cov(col, col);
            if (!(var > 0.0)) return;
            prev_carry.push_back(CarryPrior{kind, sat, signal, x_s(col), var});
        };
        for (const std::string& sat : registered_clock_sats)
            stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat));
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (parts) stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal,
                             est.phaseBiasColumn(parts->satellite, parts->signal));
        }

        // --- Trace ----------------------------------------------------------
        double sumsq = 0.0, maxabs = 0.0;
        for (const std::string& sat : registered_clock_sats) {
            const double v = x_s(est.clockColumn(sat));
            sumsq += v * v;
            maxabs = std::max(maxabs, std::abs(v));
        }
        const double rms = (n_clock > 0) ? std::sqrt(sumsq / n_clock) : 0.0;
        if (trace) {
            trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << n_present << ',' << n_clock << ',' << n_pb << ','
                  << plan.declared_dimension << ",1,1,"
                  << fmt::format("{:.4f}", rms) << ',' << fmt::format("{:.4f}", maxabs)
                  << ',' << total_pins << '\n';
        }

        if (n_emitted == 1 || n_emitted % 60 == 0) {
            log->info("Network: TOW {:.1f} emitted epoch {} — {} stations, {} sat clocks, "
                      "{} phase biases ({} single-station sat(s) dropped), datum dim {}, "
                      "clock-Δ rms {:.3f} m",
                      se.time.tow, n_emitted, n_present, n_clock, n_pb, n_dropped_sats,
                      plan.declared_dimension, rms);
        }

        prev_week = se.time.week; prev_tow = se.time.tow;
    }

    if (trace) trace.close();
    if (soln) soln.close();
    if (cl_trace) cl_trace.close();

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate) — of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, synced.size());
    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());

    // --- Closed-loop integer-recovery summary -------------------------------
    if (closed_loop_on && consumer) {
        const double fix_rate = cl_ar_ran ? (100.0 * static_cast<double>(cl_ar_fixed)
                                             / static_cast<double>(cl_ar_ran)) : 0.0;
        const double mean_fixed = cl_ar_fixed ? (static_cast<double>(cl_sum_fixed)
                                                 / static_cast<double>(cl_ar_fixed)) : 0.0;
        // Converged-window robust stats: median (typical accuracy, spike-immune)
        // + max (surfaces wrong-fix excursions the RMS would smear).
        double conv_med = 0.0, conv_max = 0.0;
        if (!cl_recent_err3d.empty()) {
            std::vector<double> w(cl_recent_err3d.begin(), cl_recent_err3d.end());
            std::sort(w.begin(), w.end());
            conv_med = w[w.size() / 2];
            conv_max = w.back();
        }
        const Eigen::Vector3d final_err = consumer->getPosition() - cl_truth;
        const Eigen::Vector3d final_enu = Coordinates::ecefToEnu(final_err, cl_truth_llh);

        const char* product_label = !cl.consume_network_products
            ? "BASELINE (precise products only — network store NOT consumed)"
            : (cl.consume_network_clock ? "FULL (network clock + phase bias)"
                                        : "BIAS-ONLY (precise clock + network phase bias)");
        const std::string mode_label =
            std::string(cl.consumer_undifferenced ? "UNDIFFERENCED" : "BSSD")
            + " user / " + product_label;

        // Honest, fix-rate-led verdict.  WL-only AR does not fix the narrow lane,
        // so the post-fix position stays at the WL-float level (~m) even on a
        // healthy product — the WL fix RATE, not the position, is the recovery
        // metric.  Flag a marginal result and point at the A/B baseline knob so a
        // reader never reads a degraded run as a clean pass.
        std::string verdict;
        if (cl_ar_fixed == 0)
            verdict = "NO integers recovered — inspect closed_loop_trace.csv / SSR coverage";
        else if (fix_rate >= 50.0)
            verdict = fmt::format("RECOVERED INTEGERS — {:.0f}% WL fix rate from our own "
                                  "network products", fix_rate);
        else
            verdict = fmt::format("PARTIAL — {:.0f}% WL fix rate (marginal); compare with the "
                                  "precise-products baseline (network.closed_loop."
                                  "consume_network_products=false)", fix_rate);

        log->info("==== Closed-loop integer-recovery harness — held-out '{}' ====",
                  cl.station.name);
        log->info("  mode                      : {}", mode_label);
        log->info("  consumer epochs processed : {}", cl_epochs);
        log->info("  AR invoked / accepted     : {} / {}  ({:.1f}% fix rate)",
                  cl_ar_ran, cl_ar_fixed, fix_rate);
        log->info("  mean WL integers per fix  : {:.1f}", mean_fixed);
        if (cl_first_fix_tow >= 0.0)
            log->info("  first integer fix at      : week {} tow {:.1f}",
                      cl_first_fix_week, cl_first_fix_tow);
        else
            log->info("  first integer fix         : NONE — no epoch recovered integers");
        log->info("  position error (final)    : {:.3f} m 3D  (E {:.3f}, N {:.3f}, U {:.3f})",
                  final_err.norm(), final_enu.x(), final_enu.y(), final_enu.z());
        log->info("  position (last {} ep)      : median {:.3f} m, max {:.3f} m 3D "
                  "(max >> median ⇒ wrong-fix excursions)",
                  cl_recent_err3d.size(), conv_med, conv_max);
        log->info("  VERDICT                   : {}", verdict);
        log->info("  closed-loop trace         : {}",
                  (log_dir_ / "closed_loop_trace.csv").string());
        log->info("==============================================================");
    }
    if (n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the datum is under-specified "
                   "(check active_systems = GPS+Galileo and ≥2 overlapping stations).");
        return 1;
    }
    return 0;
}

} // namespace GnssProcessor
