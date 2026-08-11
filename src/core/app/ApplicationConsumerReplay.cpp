/**
 * @file ApplicationConsumerReplay.cpp
 * @brief Q5 consumer-only replay driver — position-domain grading of an
 *        ALREADY-WRITTEN network product stream (no producer solve).
 *
 * The in-process closed-loop harness (`ApplicationNetworkSetup` +
 * `NetworkEmitStore`) answers "does our user engine recover integers from our
 * own products?" — but only while the producer is running, at producer cost
 * (a 305-station day is ~25 h wall).  A finished investigation instead needs
 * the position-domain consequence of a product stream that ALREADY EXISTS on
 * disk, graded per hour, in minutes.
 *
 * This driver is that path.  It is deliberately a re-wiring, not a
 * re-implementation: every consumer-side decision is the harness's.
 *
 *   harness                         | replay (here)
 *   --------------------------------|-------------------------------------
 *   consumer config                 | GnssConfig::deriveClosedLoopConsumerConfig()   (same call)
 *   consumer engine                 | RealTimeEngine + setSsrStore(owned store)      (same call)
 *   per-epoch observations          | MultiRinexSync over a ONE-station roster       (same class)
 *   per-epoch store fill            | NetworkProductReader::replay from the FILE     (vs emitter.emitToStore)
 *   trace                           | closed_loop_trace.csv, identical columns       (same schema)
 *
 * Because the store is filled through `NetworkProductReader::replay`, the
 * consumer sees the recorded stream's discontinuity counters reconstructed
 * exactly (replay's deterministic recompute + the RTA-4 serialized-counter
 * adoption), i.e. a byte-faithful reconstruction of what a live tail consumer
 * would have received.
 *
 * What this driver does NOT do: run the joint estimator, touch the producer's
 * snapshot / fingerprint, emit any product, or supply the in-process SH-VTEC
 * slant iono (that channel has no wire record — a file consumer cannot receive
 * it; `consume_network_iono` is warned and left inert).
 *
 * Armed by @c network.closed_loop.replay_products_path; @c runNetwork
 * short-circuits here before any producer setup.
 */

#include "app/Application.hpp"

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "realtime/RealTimeEngine.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Dense>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// One hour-band accumulator for the position-domain grading (handoff §4.1
/// methodology: report LEVEL BANDS per hour, never a fitted slope — refits over
/// different windows of this series gave slopes of both signs).
struct HourBand {
    std::size_t n{0};
    double sum{0.0};
    double min{std::numeric_limits<double>::max()};
    double max{-std::numeric_limits<double>::max()};
    std::vector<double> vals;   ///< kept for the median / p95 (hours are ≤120 rows)

    void add(double v) {
        ++n; sum += v;
        min = std::min(min, v);
        max = std::max(max, v);
        vals.push_back(v);
    }
    double mean() const { return n ? sum / static_cast<double>(n) : 0.0; }
    double quantile(double q) {
        if (vals.empty()) return 0.0;
        std::sort(vals.begin(), vals.end());
        const double pos = q * static_cast<double>(vals.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
        const std::size_t hi = static_cast<std::size_t>(std::ceil(pos));
        const double frac = pos - static_cast<double>(lo);
        return vals[lo] * (1.0 - frac) + vals[hi] * frac;
    }
};

} // namespace

int Application::runConsumerReplay() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();
    const ClosedLoopConfig& cl = nc.closed_loop;

    log->info("=== Q5 consumer replay: '{}' vs product file '{}' ===",
              cl.station.name, cl.replay_products_path);

    // --- Validate the consumer station ---------------------------------------
    // Same three requirements as the live harness (name + rinex_path + truth
    // ECEF); without truth there is nothing to grade against, so this is fatal
    // here rather than a "disable the harness" degrade.
    if (cl.station.name.empty() || cl.station.rinex_path.empty()
        || !cl.station.has_position) {
        log->error("Consumer replay: network.closed_loop.station is incomplete — it needs "
                   "name + rinex_path + position_ecef (the truth coordinates the position "
                   "error is graded against).");
        return 1;
    }
    if (!std::filesystem::exists(cl.replay_products_path)) {
        log->error("Consumer replay: product file '{}' does not exist.",
                   cl.replay_products_path);
        return 1;
    }
    if (!std::filesystem::exists(cl.station.rinex_path)) {
        log->error("Consumer replay: consumer RINEX '{}' does not exist.",
                   cl.station.rinex_path.string());
        return 1;
    }
    if (cl.consume_network_iono)
        log->warn("Consumer replay: closed_loop.consume_network_iono is on, but the SH-VTEC "
                  "slant iono is an IN-PROCESS supply with no product-wire record — a file "
                  "replay cannot receive it.  The setting is INERT here (the consumer floats "
                  "its own iono); this arm is therefore not comparable to an in-process "
                  "iono-supplied harness run.");

    // --- Phase 1: resolve products (SP3/CLK/ATX/BIA/nav) ---------------------
    // The consumer's a-priori orbit (and, when consume_network_clock is off, its
    // clock) is the SAME precise product the producer used; resolveData fills
    // those paths exactly as the producer path does.
    if (!resolveData())
        log->warn("Consumer replay: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config.");

    // --- Phase 2: read the product stream ------------------------------------
    // Batch read (not the tail): the file is complete and finite, and a
    // from-the-start read is what makes every discontinuity counter exact.
    SbasisContract file_contract{};
    bool products_ok = false;
    // H-rider (edge-queue §1.6 item 3b): collect the H heartbeats too.  Before
    // this, the batch replay dropped every H on the wire unread (1440/12 h
    // measured) — only the live tail surfaced them — so the consumer's
    // producer-status channel (logProducerStatus, and any future RTF-9 gating)
    // was structurally blind in replay.
    std::vector<NetworkProducerStatus> statuses;
    const auto records =
        NetworkProductReader::read(cl.replay_products_path, &file_contract, &products_ok,
                                   &statuses);
    if (records.empty()) {
        log->error("Consumer replay: product file '{}' yielded 0 epochs.",
                   cl.replay_products_path);
        return 1;
    }
    if (!products_ok)
        log->warn("Consumer replay: product file '{}' had malformed line(s); parsing "
                  "continued past them ({} epochs recovered).",
                  cl.replay_products_path, records.size());
    log->info("Consumer replay: {} product epochs parsed, tow {:.1f} → {:.1f} (week {}), "
              "S-contract {}.",
              records.size(), records.front().epoch.epoch_time.tow,
              records.back().epoch.epoch_time.tow,
              records.front().epoch.epoch_time.week,
              file_contract.present ? "present (v2)" : "ABSENT (v1 file — replay uses "
                                                       "default emitter parameters)");
    if (file_contract.present)
        log->info("Consumer replay: contract — datum receiver '{}', apriori '{}', "
                  "disc threshold {:.4f} m, cbjump {}, clock_disc {}.",
                  file_contract.decl.reference_receiver_id,
                  file_contract.apriori_product_id, file_contract.disc_threshold_m,
                  file_contract.code_bias_jump_test ? "on" : "off",
                  file_contract.emit_clock_disc ? "on" : "off");

    // --- Phase 3: the consumer's observations --------------------------------
    // MultiRinexSync with a ONE-station roster: the consumer IS the reference, so
    // synchronize() walks its own epoch schedule and by_station carries its
    // observations.  Reusing the synchronizer (rather than RinexParser directly)
    // keeps the observation vector byte-identical to what the live harness feeds.
    if (!product_parser_)
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);

    BaselineConfig roster;
    roster.epoch_sync_tolerance_s = nc.epoch_sync_tolerance_s;
    roster.reference = cl.station;

    MultiRinexSync sync(config_, roster, log_manager_, *product_parser_);
    // NO parse window.  The producer clips because it parses 305 full-day files;
    // one station is seconds.  More importantly the producer's window is anchored
    // at the FILE start, while the replay's schedule is anchored at the PRODUCT
    // stream start (a leg's products typically begin hours into the obs day) —
    // clipping from the file start would strip out exactly the epochs that carry
    // products and leave the run with nothing to consume.
    if (!sync.load()) {
        log->error("Consumer replay: failed to parse consumer RINEX '{}'.",
                   cl.station.rinex_path.string());
        return 1;
    }
    if (!sync.synchronize()) {
        log->error("Consumer replay: failed to build the synced epoch schedule.");
        return 1;
    }
    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Consumer replay: consumer RINEX yielded 0 epochs.");
        return 1;
    }
    log->info("Consumer replay: {} observation epochs, tow {:.1f} → {:.1f}.",
              synced.size(), synced.front().time.tow, synced.back().time.tow);

    // --- Phase 4: the consumer engine ----------------------------------------
    // Identical construction to ApplicationNetworkSetup's closed-loop block, so
    // the two paths cannot diverge in what "the consumer" means.
    GnssConfig consumer_config = config_.deriveClosedLoopConsumerConfig();
    consumer_config.setObservationPath(cl.station.rinex_path);

    SsrCorrectionStore store;
    NetworkProductEmitter emitter = NetworkProductReader::makeReplayEmitter(file_contract);

    RealTimeEngine consumer(consumer_config, log_manager_);
    if (!consumer.initialize()) {
        log->error("Consumer replay: consumer engine initialize() failed.");
        return 1;
    }
    consumer.setArIgnoreMwWl(!consumer_config.getArWlOnly());
    if (cl.consume_network_products) {
        consumer.setSsrStore(&store, cl.consume_network_clock, /*bias=*/true);
        log->info("Consumer replay: consumer '{}' reading the OWNED replay store ({}), "
                  "AR mode {}, wl_only {}, model {}.",
                  cl.station.name,
                  cl.consume_network_clock ? "full: network clock + phase bias"
                                           : "bias-only: precise clock + network phase bias",
                  consumer_config.getArMode(),
                  consumer_config.getArWlOnly() ? "on" : "off",
                  cl.consumer_undifferenced ? "undifferenced" : "BSSD");
    } else {
        log->info("Consumer replay: BASELINE arm — precise products only, the replay store "
                  "is NOT injected (closed_loop.consume_network_products = false).");
    }

    // --- Phase 5: trace ------------------------------------------------------
    // Same schema as NetworkRunTraces' closed_loop_trace.csv so one grading
    // script reads harness runs and replay runs alike.  Written here (not
    // through NetworkRunTraces) because that class owns the PRODUCER's stream
    // set and constructing it would demand producer state this driver has none
    // of; the header string is duplicated deliberately and asserted by the
    // grading script.
    std::filesystem::create_directories(log_dir_);
    const std::filesystem::path cl_path = log_dir_ / "closed_loop_trace.csv";
    std::ofstream cl_trace(cl_path);
    if (!cl_trace) {
        log->error("Consumer replay: failed to open {}", cl_path.string());
        return 1;
    }
    cl_trace << "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
                "ratio,pos_err_3d_m,east_m,north_m,up_m,ar_gate,p_succ\n";

    // Truth in the frame the consumer estimates: the configured coordinate is a
    // published (monument-referenced) position, the state is the ARP.  Reduce via the
    // consumer's own RINEX eccentricity so up_m carries no antenna-height offset —
    // an exact no-op on a 0/0/0 header, which keeps replayed arms comparable to the
    // live-harness numbers they are graded against.
    Eigen::Vector3d truth(cl.station.position_ecef.data());
    if (const RinexParser* cl_parser = sync.parserOf(cl.station.name)) {
        const Eigen::Vector3d hen = cl_parser->getAntennaDeltaHen();
        truth = Coordinates::markerToArp(truth, hen);
        if (hen.norm() > 1e-6)
            log->info("Consumer replay: truth reduced marker -> ARP via ANTENNA: DELTA "
                      "H/E/N ({:.4f}, {:.4f}, {:.4f}) m", hen(0), hen(1), hen(2));
    }
    const Coordinates::Llh truth_llh = Coordinates::ecefToLlh(truth);

    // --- Phase 6: the replay loop --------------------------------------------
    // Contract: the store must reflect THIS epoch's products BEFORE the consumer
    // reads them — the live harness emits then consumes within one epoch, and a
    // file consumer that lagged by one epoch would grade a 30 s-stale stream.
    // The cursor therefore replays every record up to and including the current
    // observation time (half-interval tolerance), and records are ALWAYS replayed
    // in order even across skipped/windowed-out observation epochs so the
    // emitter's cross-epoch counter state stays continuous.
    const double sync_tol = std::max(0.5, nc.epoch_sync_tolerance_s);
    const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;
    const double max_duration_s = config_.getMaxDuration();

    std::size_t rec_cursor = 0;
    std::size_t h_cursor = 0, n_h_pushed = 0;   // H-rider: heartbeat stream cursor + forwarded count
    double      last_status_pushed_s = -1.0;   // content-epoch dedup key (mirrors ApplicationRealtime)
    std::size_t n_processed = 0, n_no_products = 0, n_ar_ran = 0, n_ar_fixed = 0;
    long long   sum_fixed = 0;
    double first_fix_tow = -1.0; int first_fix_week = -1;
    double start_anchor_s = -1.0, dur_anchor_s = -1.0;
    bool   dur_anchored = false;
    std::map<int, HourBand> band_3d, band_up;
    std::deque<double> recent_err3d;
    constexpr std::size_t kRecentWindow = 30;
    double hour0_s = -1.0;

    for (const auto& se : synced) {
        const double now_s = TimeUtils::continuousGpsSeconds(se.time);

        // Nothing to consume before the product stream starts: a consumer with
        // consume_network_clock=true would run broadcast-grade (every satellite
        // dropped) and only pollute the grading.  Fast-forward instead.
        if (now_s + sync_tol
            < TimeUtils::continuousGpsSeconds(records.front().epoch.epoch_time))
            continue;

        // Producer-identical window semantics (start_offset anchors on the first
        // eligible epoch; max_duration anchors on the first PROCESSED epoch).
        if (start_offset_s > 0.0) {
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) continue;
        }
        if (max_duration_s > 0.0) {
            if (!dur_anchored) { dur_anchor_s = now_s; dur_anchored = true; }
            else if (now_s - dur_anchor_s >= max_duration_s) {
                log->info("Consumer replay: reached max processing duration ({:.1f} s of "
                          "data); stopping.", max_duration_s);
                break;
            }
        }
        LogManager::setCurrentEpochTime(se.time);

        // Advance the product stream to this observation epoch.
        std::size_t replayed_here = 0;
        while (rec_cursor < records.size()
               && TimeUtils::continuousGpsSeconds(records[rec_cursor].epoch.epoch_time)
                      <= now_s + sync_tol) {
            NetworkProductReader::replay(records[rec_cursor], store, emitter);
            ++rec_cursor;
            ++replayed_here;
        }
        // H-rider: advance the heartbeat stream in step and forward each H into
        // the owned store — the batch analogue of the RTA-6 tail wiring in
        // ApplicationRealtime (keyed on the content epoch, pushed once).  This
        // includes the standalone H's of fully WITHHELD epochs, which complete
        // no product record and are exactly when the consumer must hear about
        // the producer.  The engine reads the store surface per processed epoch
        // (logProducerStatus one-shot transitions); replay determinism of the
        // solve is unaffected — the status channel is observability, not a
        // correction.
        while (h_cursor < statuses.size()
               && TimeUtils::continuousGpsSeconds(statuses[h_cursor].time)
                      <= now_s + sync_tol) {
            const double hkey = statuses[h_cursor].time.week * 604800.0
                              + statuses[h_cursor].time.tow;
            if (hkey != last_status_pushed_s) {
                last_status_pushed_s = hkey;
                store.setProducerStatus(statuses[h_cursor]);
                ++n_h_pushed;
            }
            ++h_cursor;
        }
        if (replayed_here == 0) ++n_no_products;
        // The engine buffers observations that arrived ahead of their corrections;
        // a fresh product epoch must wake that buffer exactly as the RTA-6 tail
        // path does.
        consumer.notifyProductsAdvanced();

        auto obs_it = se.by_station.find(cl.station.name);
        if (obs_it == se.by_station.end() || obs_it->second.empty()) continue;

        consumer.processEpoch(obs_it->second);
        ++n_processed;

        const Eigen::Vector3d pos = consumer.getPosition();
        const Eigen::Vector3d d   = pos - truth;
        const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, truth_llh);
        const double err3d = d.norm();

        const bool ar_ran = consumer.lastArRan();
        const AmbiguityResolution& ar = consumer.lastArResult();
        const bool fixed = ar_ran && ar.success;
        if (ar_ran) ++n_ar_ran;
        if (fixed) {
            ++n_ar_fixed;
            sum_fixed += ar.num_fixed;
            if (first_fix_tow < 0.0) { first_fix_tow = se.time.tow; first_fix_week = se.time.week; }
        }
        recent_err3d.push_back(err3d);
        if (recent_err3d.size() > kRecentWindow) recent_err3d.pop_front();

        // Hour bands anchored on the first processed epoch (handoff §4.1).
        //
        // Banded in integer milliseconds, NOT by truncating a floating divide.
        // `now_s` is week·604800 + tow, ~1.4e9 s, where one ulp is ~0.24 µs, and
        // `tow` itself carries residue from the calendar→GPS conversion.  An
        // epoch sitting exactly on an hour boundary therefore evaluates to
        // 3599.999999999… about half the time and truncates into the PREVIOUS
        // band: the DOY-129 arms banded 121/119/120 repeating instead of
        // 120/120/120.  Rounding the elapsed time to the nearest millisecond —
        // four orders of magnitude above the residue, and finer than any real
        // epoch schedule — then dividing as integers removes the boundary case
        // entirely.  (Same class as the `start_offset_hours` = 13.0 residue
        // gotcha in SPEC_day129_frame_guards §6.)
        if (hour0_s < 0.0) hour0_s = now_s;
        const long long elapsed_ms = std::llround((now_s - hour0_s) * 1.0e3);
        const int hour = static_cast<int>(elapsed_ms / 3600000LL);
        band_3d[hour].add(err3d);
        band_up[hour].add(enu.z());

        std::string ar_gate;
        if (!ar_ran) {
            ar_gate = "not_run";
        } else switch (ar.gate_reason) {
            case 1:  ar_gate = "disabled";      break;
            case 2:  ar_gate = "min_epochs";    break;
            case 3:  ar_gate = fmt::format("sigma_gate({:.3f}m)", ar.gate_sigma_3d); break;
            case 4:  ar_gate = "no_candidates"; break;
            case 5:  ar_gate = "wl_guard";      break;
            default: ar_gate = ar.success ? (ar.wl_only_fix ? "wl_only" : "fixed")
                             : (ar.ratio > 0.0 ? "ratio_battery" : "mlambda_fail");
                     break;
        }
        cl_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                 << obs_it->second.size() << ',' << (ar_ran ? 1 : 0) << ','
                 << (fixed ? 1 : 0) << ',' << (ar_ran ? ar.num_fixed : 0) << ','
                 << (ar_ran ? ar.num_candidates : 0) << ','
                 << fmt::format("{:.2f}", ar_ran ? ar.ratio : 0.0) << ','
                 << fmt::format("{:.4f}", err3d) << ','
                 << fmt::format("{:.4f}", enu.x()) << ','
                 << fmt::format("{:.4f}", enu.y()) << ','
                 << fmt::format("{:.4f}", enu.z()) << ','
                 << ar_gate << ','
                 << fmt::format("{:.6f}", ar_ran ? ar.p_succ : 0.0) << '\n';

        if (n_processed == 1 || n_processed % 120 == 0)
            log->info("Consumer replay: ep {} (tow {:.1f}) — {} sats, 3D err {:.3f} m "
                      "(E {:.3f} N {:.3f} U {:.3f}), products replayed {}/{}.",
                      n_processed, se.time.tow, obs_it->second.size(), err3d,
                      enu.x(), enu.y(), enu.z(), rec_cursor, records.size());
    }
    cl_trace.flush();

    // --- Phase 7: run-end telemetry ------------------------------------------
    // Counters, not the absence of crashes (standing invariant #4): a replay that
    // processed epochs but never advanced the product cursor is INERT and must
    // read as a failure, not as a clean run.
    if (n_processed == 0) {
        log->error("Consumer replay: 0 epochs processed — no observation epoch overlapped "
                   "the product stream.  Check the station's date and the config window.");
        return 1;
    }
    // H-rider (invariant #4): the heartbeat channel's own counter.  "0 of N
    // forwarded" with H records on the wire means the interleave is mis-wired;
    // "0 on the wire" means the producer ran without emit_epoch_status_record.
    log->info("Consumer replay: H heartbeats — {} on the wire, {} forwarded to the "
              "consumer store (batch analogue of the RTA-6 tail surface).",
              statuses.size(), n_h_pushed);
    if (rec_cursor == 0) {
        log->error("Consumer replay: 0 product records replayed over {} processed epochs — "
                   "the store was never filled, so this run graded plain PPP, not the "
                   "network stream.", n_processed);
        return 1;
    }

    double recent_rms = 0.0;
    for (double v : recent_err3d) recent_rms += v * v;
    recent_rms = recent_err3d.empty() ? 0.0
                                      : std::sqrt(recent_rms / static_cast<double>(recent_err3d.size()));

    log->info("Consumer replay COMPLETE: {} epochs processed, {} product records replayed "
              "({} epochs had no fresh product), AR ran {} / fixed {} (mean {:.1f} sats), "
              "first fix {}, last-{} 3D RMS {:.4f} m.",
              n_processed, rec_cursor, n_no_products, n_ar_ran, n_ar_fixed,
              n_ar_fixed ? static_cast<double>(sum_fixed) / static_cast<double>(n_ar_fixed) : 0.0,
              first_fix_tow < 0.0 ? std::string{"none"}
                                  : fmt::format("week {} tow {:.1f}", first_fix_week, first_fix_tow),
              recent_err3d.size(), recent_rms);

    // Hourly LEVEL bands — the only defensible summary of this series (handoff
    // §4.1: a single Theil-Sen slope over it is invalid).
    log->info("Consumer replay hourly bands (hour from first processed epoch):");
    log->info("  hour  n     3D mean   3D med    3D p95    3D max    up mean   up min    up max");
    std::ofstream bands(log_dir_ / "consumer_replay_hourly.csv");
    if (bands)
        bands << "hour,n,err3d_mean_m,err3d_median_m,err3d_p95_m,err3d_max_m,"
                 "up_mean_m,up_min_m,up_max_m\n";
    for (auto& [hour, b] : band_3d) {
        auto& u = band_up[hour];
        const double med = b.quantile(0.5), p95 = b.quantile(0.95);
        const double umean = u.mean();
        log->info("  {:>4}  {:>4}  {:>8.3f}  {:>8.3f}  {:>8.3f}  {:>8.3f}  {:>8.3f}  "
                  "{:>8.3f}  {:>8.3f}",
                  hour, b.n, b.mean(), med, p95, b.max, umean, u.min, u.max);
        if (bands)
            bands << hour << ',' << b.n << ','
                  << fmt::format("{:.4f}", b.mean()) << ','
                  << fmt::format("{:.4f}", med) << ','
                  << fmt::format("{:.4f}", p95) << ','
                  << fmt::format("{:.4f}", b.max) << ','
                  << fmt::format("{:.4f}", umean) << ','
                  << fmt::format("{:.4f}", u.min) << ','
                  << fmt::format("{:.4f}", u.max) << '\n';
    }

    return 0;
}

} // namespace GnssProcessor
