/**
 * @file RealTimeEngine.hpp
 * @brief Real-time GNSS processing engine.
 *
 * Owns the full processing pipeline (state manager, PPP model, Kalman filter,
 * WLS estimator, slip detectors) and processes epochs one-at-a-time as they
 * arrive from a live data stream.  The engine is transport-agnostic: the
 * Application layer feeds it complete RTCM frames which it decodes, converts
 * to RinexObservation/RtcmSsrCorrection and runs through the same PPP
 * algorithm used by post-processing.
 *
 * Lifetime:
 *   1. Construct with config + log manager.
 *   2. Call initialize() to set up products, filter, models.
 *   3. Call feedRtcmFrame() each time a complete frame is available.
 *   4. processEpoch() runs when a full epoch of observations is buffered:
 *      feedRtcmFrame() calls it at each epoch boundary, and flush() processes
 *      the last buffered epoch after the stream ends.
 *   5. Query latest state via getState() / getPosition().
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/KalmanFilter.hpp"
#include "aiding/AidingSourceManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "slip/MwSlipDetector.hpp"
#include "slip/TurboEditResolver.hpp"
#include "slip/SoftLliBridge.hpp"
#include "filter/AmbiguityResolver.hpp"
#include "model/PppModel.hpp"
#include "common/PppTypes.hpp"
#include "common/PppObservation.hpp"     // RtEpochContext::modeled_obs element type
#include "common/EpochMeasurement.hpp"   // RtEpochContext::meas_update
#include "common/GnssTypes.hpp"          // DropReason (RtEpochContext::drop_reasons)
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "rtcm/RtcmParser.hpp"
#include "rtcm/LockTimeTracker.hpp"
#include "sbf/SubframeAssembler.hpp"
#include "filter/StateManager.hpp"
#include "logging/TraceLogger.hpp"
#include "math/WlsEstimator.hpp"
#include "math/TimeUtils.hpp"
#include "products/SsrCorrectionStore.hpp"   // SsrCorrectionStore + NetworkProducerStatus (RTA-10)
#include "realtime/PbDiscCarryLedger.hpp"    // Declared-step ambiguity carry ledger, card CC (A)
#include "realtime/AmbLevelCheck.hpp"        // Consumer ambiguity level re-validation
#include "realtime/PbStepTranslateLedger.hpp"  // Every-step ambiguity translation ledger, card PBT (A)
#include "realtime/PbHoldLedger.hpp"         // Held phase-bias generation ledger
#include "realtime/ConsumerSatTrace.hpp"     // Per-satellite consumer readout row
#include <Eigen/Dense>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief Self-contained real-time PPP processing engine.
 *
 * Encapsulates all processing components and exposes a simple frame-in /
 * state-out interface suitable for streaming operation.
 */
class RealTimeEngine {
public:
    /**
     * @brief Construct the engine.
     * @param config  Loaded GNSS configuration.
     * @param log_manager Application log manager.
     */
    RealTimeEngine(GnssConfig& config, LogManager& log_manager);

    /// Out-of-line (RealTimeEngine.cpp): the staged_rt_snap_ unique_ptr holds a
    /// forward-declared RtStateSnapshot, so the destructor must be emitted where
    /// the complete type is visible.
    ~RealTimeEngine();

    /**
     * @brief Initialize products, filter and models.
     *
     * Must be called once before processing any data.  Loads static
     * products (ATX, BIA, broadcast nav) needed for real-time operation.
     *
     * @return true on success.
     */
    bool initialize();

    /**
     * @brief Per-stream SSR ingest accept policy (RTA-8(b)).
     *
     * Applied per frame on the SSR push path in @ref feedRtcmFrame: the driver
     * (@c Application::runRealTime) passes the policy of the stream the frame
     * came from, so each stream can have its own filter.  Configured per
     * corrections/ephemeris stream with the @c corrections_accept key
     * (@c realtime.corrections.corrections_accept /
     * @c realtime.ephemeris.corrections_accept).
     *
     * @c OrbitOnly serves the wiring that takes orbits from the external RTS
     * stream and clocks and biases from the network product.  Without it, the
     * RTS stream's 1060/1066 combined records would put clock content in the
     * singleton and shadow the network clock wherever the singleton is
     * consulted.  Under @c OrbitOnly the stream's orbit and URA records are
     * pushed unchanged, standalone clock / code-bias / phase-bias /
     * high-rate-clock records are dropped, and combined orbit+clock records are
     * decomposed into an orbit-only push.  The store's combined-wins merge logic
     * is not involved: the combined record never enters the store.
     */
    enum class SsrAcceptPolicy {
        All,       ///< Push every decoded SSR record (default; byte-identical legacy path).
        OrbitOnly  ///< Push orbit + URA; drop clock/bias content; decompose combined → orbit.
    };

    /// Kinds of decoded SSR record the push path routes, one-to-one with the
    /// RtcmParser getters and the SsrCorrectionStore push API (RTA-8(b)).
    enum class SsrRecordKind {
        Orbit, Clock, Combined, CodeBias, PhaseBias, Ura, HighRateClock
    };

    /// What the push site does with one record under a policy (RTA-8(b)).
    enum class SsrPushAction {
        Push,           ///< Push the record unchanged.
        Drop,           ///< Do not push.
        DecomposeOrbit  ///< Combined under OrbitOnly: push only the orbit part (see combinedOrbitPart).
    };

    /**
     * @brief Ingest filter: maps a record kind to a push action (RTA-8(b)).
     *
     * Pure function, kept out of @ref feedRtcmFrame so the policy matrix is
     * unit-testable.  @c All → @c Push for every kind.  @c OrbitOnly →
     * @c Push for Orbit/Ura, @c DecomposeOrbit for Combined, @c Drop for
     * Clock/CodeBias/PhaseBias/HighRateClock.  The high-rate clock is clock
     * content (an additive delta on the clock polynomial), so it is dropped
     * with the rest of the clock family.
     */
    static SsrPushAction ssrPushAction(SsrAcceptPolicy policy, SsrRecordKind kind);

    /**
     * @brief Extract the orbit-only part of a combined orbit+clock record (RTA-8(b)).
     *
     * Copies sat / IODE / RAC delta + rate / received & content epochs /
     * IOD-SSR / reference-point tag into an @c SsrOrbitCorrection and discards
     * c0/c1/c2.  Pushing the result via @c SsrCorrectionStore::pushOrbit yields a
     * record whose @c getOrbitClock merge reports @c has_orbit=true,
     * @c has_clock=false.  The decomposition happens at the push site; the
     * store's combined-wins preference is left unchanged.
     */
    static SsrOrbitCorrection combinedOrbitPart(const SsrCombinedCorrection& rec);

    /**
     * @brief Parse a @c corrections_accept config string (RTA-8(b)).
     *
     * @c "all" (or empty, the unset default) → @c All; @c "orbit_only" →
     * @c OrbitOnly; anything else → @c All with @p recognized (when non-null)
     * set false so the caller can warn once.  Case-insensitive.
     */
    static SsrAcceptPolicy parseSsrAcceptPolicy(const std::string& value,
                                                bool* recognized = nullptr);

    /**
     * @brief Feed a complete RTCM3 frame for decoding.
     *
     * Decoded observations are buffered internally.  When a new epoch
     * boundary is detected the previous epoch is automatically submitted
     * for processing via processEpoch().
     *
     * @param frame  Complete RTCM3 frame (header + payload + CRC).
     * @param accept SSR ingest policy for the stream this frame came from
     *        (RTA-8(b)).  The default, @c All, pushes every record, which keeps
     *        a caller that passes no policy byte-identical.  Observations,
     *        broadcast nav and station coordinates are never filtered; the
     *        policy acts only on the SSR push loop.
     * @param receipt Steady-clock receipt stamp from the framer boundary
     *        (@c RtcmStreamReader::lastFrameReceipt) for sub-second
     *        receive→process latency accounting (RTB-1(b)).  The stamp of the
     *        first frame that contributes observations to a buffered epoch is
     *        carried to that epoch's processEpoch and reported in the
     *        obs-backlog diagnostics; it never gates control flow.  Defaults to
     *        "now" (a caller without a framer stamp loses only the framer→feed
     *        skew).
     */
    void feedRtcmFrame(const std::vector<uint8_t>& frame,
                       SsrAcceptPolicy accept = SsrAcceptPolicy::All,
                       std::chrono::steady_clock::time_point receipt =
                           std::chrono::steady_clock::now());

    /**
     * @brief Feed a complete Septentrio SBF block for decoding.
     *
     * The SBF analogue of @c feedRtcmFrame: a MeasEpoch block carries a full
     * epoch of observations and is processed immediately; broadcast-ephemeris
     * blocks (GPSNav/GALNav/BDSNav/QZSNav/GLONav) are pushed into the live
     * ephemeris store.  SSR corrections still arrive via the RTCM corrections
     * stream; SBF carries only the rover observations and nav.
     *
     * @param block  Complete SBF block (Sync + CRC + ID + Length + body).
     */
    void feedSbfFrame(const std::vector<uint8_t>& block);

    /**
     * @brief Feed a set of observations directly (e.g. from RTCM replay file).
     *
     * @param observations  Observations for one epoch.
     */
    void feedObservations(const std::vector<RinexObservation>& observations);

    /**
     * @brief Feed one streaming IMU sample (real-time INS aiding).
     *
     * Buffers the sample into the IMU aiding source (if one is active); it is
     * consumed by the next epoch's @c predictAided over the (prev, current]
     * interval.  No-op when no IMU source is configured, so a GNSS-only stream
     * is unaffected.  The real-time IMU path is wired end to end but not yet
     * validated against live IMU data (the rooftop AsteRx-U is GNSS-only); see
     * docs/aiding_source_abstraction_design.md.
     *
     * @param sample Streaming aiding sample (kind == Imu).
     */
    void feedImuSample(const AidingSample& sample);

    /**
     * @brief Process any epoch still buffered for assembly.
     *
     * Epoch flushing in @c feedRtcmFrame is edge-triggered: a buffered epoch
     * is only submitted when the next epoch's first observation arrives.  On
     * stream stop / disconnect the final buffered epoch would otherwise be
     * dropped.  Call this once after the stream(s) end to process it.
     */
    void flush();

    /**
     * @brief Process a single epoch of observations through the PPP pipeline.
     *
     * Steps: admission gates -> modeled observations -> WLS clock seed ->
     * KF predict -> cycle-slip detection -> SSR datum-break reactions ->
     * KF update (with measurement rebuild passes) -> ZWD and iono
     * pseudo-observation constraints -> AR -> state extraction and traces.
     *
     * @param epoch_obs Observations for this epoch.
     * @return true if the epoch was processed successfully.
     */
    bool processEpoch(const std::vector<RinexObservation>& epoch_obs);

    /**
     * @brief Where the SSR orbit overlay reads from when a store is injected via
     *        @ref setSsrStore (RTA-8(a)).
     *
     * @c FollowClock, the default, routes the orbit overlay with the clock (per
     * @c consume_clock), so the closed-loop harness call site keeps its behavior.
     * The standalone network-consumer wiring selects @c External instead (the
     * default of @c processing.network_consumer.orbit_source): the orbit overlay
     * stays on the process-wide singleton, fed by the external RTS corrections
     * stream (ideally ingest-filtered to @c orbit_only, RTA-8(b)), while clocks
     * and biases come from the injected network-product store.  @c Network is
     * for a future product that carries its own orbit corrections.
     */
    enum class SsrOrbitSource {
        FollowClock,  ///< Legacy coupling: orbit overlay routed with the clock (per consume_clock).
        External,     ///< Orbit overlay reads the singleton (external RTS stream); clocks per consume_clock.
        Network       ///< Orbit overlay reads the injected store (orbit-bearing network product).
    };

    /**
     * @brief Inject a non-owning SSR-correction store override (DI seam).
     *
     * Repoints the engine's SSR consumption (satellite clock via the PppModel
     * geometry/corrections, satellite code/phase bias via the MeasurementModel,
     * the phase-bias-discontinuity → ambiguity-reset hook, and the coverage
     * diagnostics) at @p store instead of the process-wide singleton.  The
     * PPP-RTK network closed-loop harness drives a held-out station through this
     * engine reading the producer's owned store (see ClosedLoopConfig), and
     * @c Application::runRealTime injects the store it fills from a tailed
     * network product.  Call after @ref initialize (so the models exist).
     * @c nullptr restores the singleton.  RTCM/SBF pushes are unaffected (they
     * always target the singleton), so the override changes consumption only.
     *
     * @param store Non-owning store pointer (must outlive the engine) or nullptr.
     * @param consume_clock  Route the SSR clock overlay (PppModel geometry +
     *        corrections) at @p store.  Set false to keep precise SP3/CLK for the
     *        clock while still consuming the satellite phase bias: an
     *        integer-recovery test that isolates the phase-bias product from a
     *        marginal network clock product.
     * @param consume_bias   Route the SSR code/phase bias (MeasurementModel) and
     *        the phase-bias-discontinuity reset hook at @p store.
     * @param orbit_source   Orbit-overlay routing (RTA-8(a)).  The default,
     *        @c FollowClock, routes the orbit with the clock (per
     *        @p consume_clock), so a caller that does not pass it (the
     *        closed-loop harness in particular) keeps the coupled routing and
     *        stays byte-identical.  See
     *        @ref SsrOrbitSource.
     */
    void setSsrStore(const SsrCorrectionStore* store,
                     bool consume_clock = true, bool consume_bias = true,
                     SsrOrbitSource orbit_source = SsrOrbitSource::FollowClock);

    /// Result of the ambiguity-resolution attempt at the most recent epoch that
    /// reached the estimation tail (closed-loop harness metric).  @ref lastArRan
    /// reports whether AR ran at that epoch (resolver present and a float update
    /// committed).
    const AmbiguityResolution& lastArResult() const { return last_ar_result_; }
    bool lastArRan() const { return last_ar_ran_; }

    /// Suppress the MW wide-lane feed into ambiguity resolution, so AR runs plain
    /// MLAMBDA on the Kalman float.  The network closed-loop validation uses this
    /// with full LAMBDA: the KF float carries the consumed network satellite phase
    /// bias, while the MW detector's WL integers are in the separate .BIA frame,
    /// and feeding them would re-inject a conflicting WL datum.  Default false
    /// (normal AR uses the MW WL to halve the search).  JSON keys (RTA-7):
    /// @c processing.ar_ignore_mw_wl (standalone) and the
    /// @c processing.network_consumer profile (see
    /// @c GnssConfig::networkConsumerArIgnoreMwWl), applied in
    /// @c Application::runRealTime.
    void setArIgnoreMwWl(bool ignore) { ar_ignore_mw_wl_ = ignore; }

    /// @brief Arm consumption of the network's undifferenced per-satellite slant
    ///        ionosphere as an ionosphere-weighted (BLUP) prior.
    ///
    /// When on, @ref processEpoch (after the measurement update, before AR)
    /// queries the injected SSR store's slant-iono channel
    /// (@c SsrCorrectionStore::getSlantIono) for each tracked satellite and pulls
    /// the matching per-satellite slant-iono state toward the supplied value
    /// through a common-mode-free pseudo-observation.  This replaces the loose
    /// @c ionosphere_q random walk with a strong (ionosphere-corrected) user model.
    /// Reads the store routed by @ref setSsrStore's @c consume_bias (the
    /// closed-loop consumer always routes bias, so this is the owned network
    /// store).  Off by default; while off, no pseudo-observation is built and the
    /// epoch is byte-identical.  Set by the closed-loop harness from
    /// @c closed_loop.consume_network_iono.
    void setConsumeNetworkIono(bool on) { consume_network_iono_ = on; }

    /**
     * @brief Product-arrival wake for the obs↔SSR hold buffer (RTA-6).
     *
     * Without this call, a held epoch (processing.enable_obs_ssr_sync) is
     * re-attempted only after an RTCM SSR push in @c feedRtcmFrame.  Network
     * products injected through @ref setSsrStore arrive outside that path (the
     * @c runRealTime driver polls a @c NetworkProductTail and pushes epochs into
     * the injected store directly), so the driver calls this after delivering at
     * least one new product epoch to run the same drain check (@c drainHeldObs).
     * No-op when nothing is held (every non-sync run), like the RTCM trigger
     * site.
     */
    void notifyProductsAdvanced() { drainHeldObs(); }

    // --- Per-satellite consumer readout ----------------------------
    /**
     * @brief Arm the per-satellite readout of this engine's own epoch
     *        (@c network.trace_consumer_sat, set by the closed-loop harness and by
     *        the consumer replay driver).
     *
     * When armed, every @c processEpoch that reaches the estimation tail leaves
     * one @ref ConsumerSatRow per (satellite, raw phase signal) in
     * @ref consumerSatRows, and the SSR datum-break reactions and the ambiguity
     * level check also record what they did per key so the row can report it.
     * The harness reads the buffer right after @c processEpoch returns and
     * writes it through @c NetworkRunTraces, which owns the stream as it does
     * for @c closed_loop_trace.csv.
     *
     * Measure-only: the harvest reads state the epoch has already committed and
     * feeds nothing back into the filter, so the consumer's solve is
     * byte-identical with the readout armed.  Off by default; while off, no
     * buffer is filled, no event is recorded and an epoch costs what it did
     * before.
     *
     * @param on Arm (true) or disarm (false) the readout.
     */
    void setConsumerSatTrace(bool on) { consumer_sat_trace_ = on; }

    /**
     * @brief The rows harvested by the most recent @c processEpoch call.
     *
     * Cleared at the top of every @c processEpoch, so an epoch that ends before
     * the estimation tail (for example at an admission gate) leaves the buffer
     * empty, since that epoch produced no consumer solution, instead of
     * repeating the previous epoch's rows.  Always empty with
     * @ref setConsumerSatTrace off.
     *
     * @return Const reference to the engine-owned row buffer.
     */
    const std::vector<ConsumerSatRow>& consumerSatRows() const { return consumer_sat_rows_; }

    /// Get the current receiver position (ECEF, meters).
    Eigen::Vector3d getPosition() const;

    /// Get the current receiver clock bias (meters).
    double getClockBias() const;

    /// Get the current ZWD estimate (meters).
    double getTroposphereZwd() const;

    /**
     * @brief Receiver inter-system bias per non-GPS system.
     *
     * Reads the Kalman state and its variance (the consumer carries the bias as a
     * state with the @c filter.process_noise.system_time_offset_q random walk).
     * Measure-only; nothing here touches the filter.
     *
     * @return System name ("Galileo") -> (value in metres, formal standard
     *         deviation in metres).  Empty when only GPS is active or before the
     *         state manager exists.
     */
    std::map<std::string, std::pair<double, double>> getSystemTimeOffsets() const;

    /// Get the full PPP state.
    const PppState& getState() const { return ppp_state_; }

    /// Number of epochs processed so far.
    uint64_t epochCount() const { return epoch_count_; }

    /// True once the processing.max_duration_s data-time budget has been
    /// reached (latched).  The real-time poll loop checks this to end the
    /// session cleanly, mirroring how post-processing breaks its epoch loop.
    /// Always false when max_duration_s is 0 (the default, unlimited).
    bool maxDurationReached() const { return max_duration_reached_; }

    // --- Declared-step ambiguity carry readout, card CC (A) ------------------
    /// True iff @c processing.ssr_phase_bias_disc_carry is armed on this engine's
    /// config (the closed-loop harness derives it from
    /// @c network.closed_loop.pb_disc_carry).
    bool pbDiscCarryArmed() const { return config_.ssrPhaseBiasDiscCarry(); }
    /// Run-end counters of the carry.  All zero, and never printed, with the
    /// knob off, so the off path stays byte-identical.
    const PbDiscCarryLedger::Counters& pbDiscCarryCounters() const {
        return pb_disc_carry_.counters();
    }

    // --- Phase-bias step translation readout, card PBT (A) -------------------
    /// True iff @c processing.ssr_phase_bias_step_translate is armed on this
    /// engine's config (the closed-loop harness derives it from
    /// @c network.closed_loop.pb_step_translate).
    bool pbStepTranslateArmed() const { return config_.ssrPhaseBiasStepTranslate(); }
    /// Run-end counters of the step translation.  All zero, and never printed,
    /// with the knob off, so the off path stays byte-identical.
    const PbStepTranslateLedger::Counters& pbStepTranslateCounters() const {
        return pb_step_translate_.counters();
    }

    // --- Consumer phase-bias hold reaction readout ----------------
    /// Run-end counters of the reaction to a held phase bias.  All zero on an
    /// unheld product, so a consumer can read back that the producer's hold was
    /// inert (invariant #4): @c cells == 0 on a clean wire means nothing was
    /// held, not that the hold worked.
    const PbHoldLedger::Counters& pbHoldCounters() const { return pb_hold_.counters(); }

    /**
     * @struct PbHoldPresenceCounters
     * @brief Presence edges of held phase-bias keys, counted whether or
     *        not @c processing.ssr_pb_hold_presence_flip is armed.
     *
     * A key is "held" here once the hold ledger has recorded it, so an edge that
     * happens on the epoch the entry leaves the store is still attributed to the
     * hold.  @c answered counts the edges that took a reaction; with the knob off
     * it stays 0 and @c appear + @c vanish is the number of full-bias steps the
     * consumer absorbed unanswered (invariant #7, both edges).
     */
    struct PbHoldPresenceCounters {
        std::size_t appear{ 0 };     ///< Held key applied now, not applied at the previous epoch.
        std::size_t vanish{ 0 };     ///< Held key applied at the previous epoch, not applied now.
        std::size_t answered{ 0 };   ///< Edges that took the A5 reaction (knob armed).
        std::size_t suppressed{ 0 }; ///< Edges left alone by the declared-roster rule (card SID 5b).
    };
    /// Run-end counters of the held-key presence edges.
    const PbHoldPresenceCounters& pbHoldPresenceCounters() const { return pb_hold_presence_; }

    /**
     * @brief Log the held-key presence-edge counters on @c rt_engine.
     *
     * Silent when no held entry was ever seen, so a consumer of an unheld product
     * keeps a byte-identical log.  Public so the consumer replay driver, which never
     * calls @ref flush, can print it.
     *
     * @param tag Where the line comes from ("end of run", "closed loop", "end of replay").
     */
    void logPbHoldPresenceSummary(const char* tag) const;

    // --- Declared signal roster at the consumer (card SID phase 5) -----------
    /**
     * @struct RosterConsumerCounters
     * @brief The consumer's half of card SID phase 5, one named counter per class.
     *
     * Every class carries a denominator so that a zero can be interpreted:
     * @c epochs_declared at 0 means no declaration ever reached the store, which
     * reads as an inert knob, not as agreement with anything.
     */
    struct RosterConsumerCounters {
        long long epochs{ 0 };            ///< epochs the refresh ran (denominator).
        long long epochs_declared{ 0 };   ///< of those, with a declaration in the store.
        long long decl_changes{ 0 };      ///< times the declaration in the store changed.

        // --- Freshness flips, with identity (E3) ----------------------------
        long long flips{ 0 };             ///< freshness flips seen (denominator).
        long long flips_phase{ 0 };
        long long flips_code{ 0 };
        long long flip_to_stale{ 0 };     ///< of those, fresh -> stale.
        long long flip_to_fresh{ 0 };     ///< of those, stale -> fresh.
        /// The three-way attribution of a flip, in the order the split is read:
        long long flip_undeclared{ 0 };   ///< the declaration names no family for this signal (structurally absent).
        long long flip_off_wire{ 0 };     ///< it does, but the store holds no record for this satellite (the family is absent here).
        long long flip_stale{ 0 };        ///< a record exists and the gate refused it on age: a genuine staleness.
        long long flip_no_decl{ 0 };      ///< no declaration was held, so the flip cannot be attributed at all.
        long long flip_events{ 0 };       ///< ARC rows written for the flips (up to a per-run cap; the rest count in flip_events_suppressed).
        long long flip_events_suppressed{ 0 };
        // --- Responses the declaration changed (C2) --------------------------
        long long flip_suppressed{ 0 };   ///< flips whose reset / re-inflation card SID phase 5b withheld.
    };
    /// The consumer's declaration census (card SID phase 5).  On a wire that
    /// carries no declaration (every producer before card SID), @c epochs_declared
    /// and @c decl_changes stay zero and every flip counts as @c flip_no_decl.
    const RosterConsumerCounters& rosterConsumerCounters() const {
        return roster_consumer_;
    }
    /// The consumer's latch against the declaration, as the model counted it
    /// (card SID phase 5).  @c compared 0 means no declaration reached the latch.
    const PppModel::DeclaredLatchCounters& declaredLatchCounters() const;
    /// One INFO block with both censuses (card SID phase 5).  Silent when no
    /// declaration was ever seen and @c network.closed_loop.roster_latch is off,
    /// so a consumer of a wire without a declaration prints nothing.
    void logRosterConsumerSummary(const char* tag) const;

    // --- Consumer ambiguity level check readout --------------------
    /// True iff @c processing.amb_level_check is armed on this engine's config
    /// (the closed-loop harness derives it from
    /// @c network.closed_loop.amb_level_check).
    bool ambLevelCheckArmed() const { return config_.ambLevelCheck(); }
    /// Run-end counters of the level check.  All zero, and never printed, with
    /// the knob off, so the off path stays byte-identical.
    const AmbLevelCheck::Counters& ambLevelCheckCounters() const {
        return amb_level_check_.counters();
    }
    /// This epoch's AMB_LEVEL events (trips and refloats), for the harness to
    /// drain into @c network_events.csv.  Cleared at the top of every
    /// @c processEpoch; empty on an unarmed engine.
    const std::vector<AmbLevelEvent>& ambLevelEvents() const { return amb_level_events_; }
    /// This epoch's full-window evaluations (@c n_amb_level_checks), for the same
    /// drain.  0 on an unarmed engine and on any epoch that did not update.
    uint64_t ambLevelEpochChecks() const { return amb_level_epoch_checks_; }

    // --- Harness-driven snapshot write and restore readout, card CC (B) ------
    /// Epoch time of the last epoch that ran the full estimation tail (the
    /// epoch the live filter memory is "as of"); nullopt before the first.
    std::optional<GnssTime> lastProcessedEpochTime() const {
        if (!has_processed_epoch_) return std::nullopt;
        return last_processed_epoch_;
    }

    /**
     * @brief Write the live filter-state snapshot to @p path immediately, with no
     *        cadence gate and no @c realtime.state_snapshot_path needed.
     *
     * Reuses @c writeStateSnapshot_'s harvest (dynamic layout verified gapless)
     * and the atomic temp-file + rename write.  The closed-loop harness calls
     * it beside every network history snapshot copy
     * (@c network.closed_loop.state_snapshot) so a resume leg's consumer can
     * warm-start at the seam.
     *
     * @param path           Destination file.
     * @param expected_epoch When non-null, the snapshot is written only if the
     *        engine's last processed epoch is this epoch (same week, |Δtow| ≤
     *        1 ms), so the consumer file carries the network copy's tow;
     *        otherwise nothing is written and @p err says why.
     * @param err            Optional one-line refusal / failure reason.
     * @return true iff a snapshot was written.
     */
    bool writeStateSnapshotTo(const std::filesystem::path& path,
                              const GnssTime* expected_epoch = nullptr,
                              std::string* err = nullptr);

    /// Path of the consumer snapshot that sits beside a network history copy
    /// `<network_snapshot_path>.ep<NNNN>`: `<network_snapshot_path>.consumer.ep<NNNN>`
    /// (the same zero-padded @c n_emitted suffix the network copy uses; card CC (B)).
    static std::filesystem::path consumerSnapshotHistoryPath(
        const std::filesystem::path& network_snapshot_path, unsigned long long n_emitted);

    /// Outcome of the one-shot staged warm resume (RealTimeEngineStateSnap.cpp),
    /// so a harness or test can tell a restore from a cold start without parsing
    /// the log.
    struct RtSnapshotRestoreStatus {
        enum class Outcome {
            NotStaged,   ///< No snapshot was staged (path empty, file absent or unparsable); the engine starts cold.
            Restored,    ///< The staged snapshot was applied at the top of the first processEpoch call.
            Discarded    ///< The staged snapshot was refused (age, fingerprint or layout gate); the engine starts cold.
        };
        Outcome     outcome{ Outcome::NotStaged };
        int         week{ 0 };            ///< Snapshot epoch (valid when Restored).
        double      tow{ 0.0 };
        std::size_t n_states{ 0 };        ///< Snapshot state size (Restored).
        std::size_t n_dynamic{ 0 };       ///< Snapshot dynamic-state count (Restored).
        std::string scope;                ///< "core" | "full" (Restored).
        std::string reason;               ///< The refusal reason (Discarded).
    };
    const RtSnapshotRestoreStatus& stateSnapshotRestoreStatus() const { return rt_snap_restore_; }

private:
    friend struct AmbLevelArcTestAccess;  ///< Exercises the ALC seam after real reseed/update recovery.
    friend struct PbHoldPresenceTestAccess;  ///< Drives the phase-bias reactions across a held key's presence edges.
    /// Convert GPS-scale time to UTC-scale time.
    static GnssTime gpsToUtcTime(const GnssTime& gps_time);

    /// Resolve the active SSR store: the injected override when set, else the
    /// process-wide singleton.  Defined in the .cpp (full type visible there).
    const SsrCorrectionStore& ssrStore() const;

    /// @brief Re-attempt the held obs↔SSR-sync epoch after the SSR stream advances.
    ///
    /// Called after an SSR push in @c feedRtcmFrame and from
    /// @c notifyProductsAdvanced.  Moves @c held_obs_ back through
    /// @c processEpoch with the sync gate re-evaluated: if the freshest SSR now
    /// covers the held epoch it is processed, otherwise it is re-held.  No-op
    /// when nothing is held (the common case and all non-sync runs).
    void drainHeldObs();

    /// @brief Log the one-line SSR bias attribute-mismatch summary (RTA-13).
    ///
    /// Reads @c ssrStore().biasAttributeMismatchCounts() and logs one INFO line
    /// with per-domain totals + the top offending (sat|signal|domain) keys.
    /// Silent when no mismatch was ever counted (every clean run).  Called
    /// periodically from @c processEpoch (every 300 processed epochs) and once
    /// from @c flush() (end-of-run).
    void logBiasAttrMismatchSummary(const char* tag);

    /// @brief Log the one-line orbit/clock coherence-echo mismatch summary (card 7).
    ///
    /// Reads @c orbitEchoMismatchCounts() from the clock-routed store
    /// (@c clock_store_override_, else the singleton), where GnssCorrections
    /// tallies them, and logs one INFO line with the total + the top offending
    /// (sat|reason) keys (reason ∈ noorbit / iod / skew) and whether the
    /// withhold budget is armed.  Silent when no mismatch was ever counted
    /// (every clean run).  Called from the same two hooks as the RTA-13 summary:
    /// periodically from @c processEpoch and once from @c flush() (end-of-run).
    void logOrbitEchoMismatchSummary(const char* tag);

    // =====================================================================
    // processEpoch seams
    // =====================================================================
    //
    // @c processEpoch is a thin ordered driver over the single-concern seams
    // below (SSR sync, ephemeris reaction, measurement build, filter update, AR
    // and emit), the same decomposition @c Application::runNetwork received in
    // INC-0..12.  Work from the RT service queue (RTF-4..9, 305-station scale)
    // goes into these seams, not into the driver.
    //
    // Seam contract (adapted from the network one; see the network-producer
    // "where new code goes" section of CLAUDE.md):
    //
    //  1. Seams are private member functions, not free functions over a context
    //     struct.  The network needed @c NetworkRunContext because @c runNetwork
    //     kept its state in function locals; every persistent value here is
    //     already a @c RealTimeEngine member, so a member function reaches
    //     @c state_manager_ / @c config_ / @c ssrStore() with no plumbing.
    //  2. Each relocated seam opens with an alias preamble reconstructing the
    //     driver's local names (@c log, @c epoch_prefix, @c modeled_obs, …) so
    //     the body reads as it did in the driver.  This makes the relocation
    //     mechanically verifiable: @c scripts/rt_seam_verify.py reconstitutes
    //     each body and asserts a character-exact match against the frozen
    //     pre-cut source.
    //  3. A seam owns no state.  Cross-seam values travel in @ref RtEpochContext,
    //     which lives on the stack for one @c processEpoch call.
    //  4. @c void seams cannot end the epoch.  @c bool seams return
    //     false == "epoch is done, return false to the caller"; the driver reads
    //     @c if (!seam(ctx)) return false.  Only the four admission gates are
    //     @c bool: every seam from the SSR coverage census onward is
    //     unconditional straight-line code, so it can be @c void.
    //
    // Where new code goes: a new per-epoch QC/estimation step goes in the seam
    // matching its concern or in a new seam, not inline in the driver.
    // Admission/deferral → @c RealTimeEngineAdmit.cpp; observability →
    // @c RealTimeEngineSsrDiag.cpp; measurement-set conditioning →
    // @c RealTimeEnginePrepare.cpp; slip/repair → @c RealTimeEngineSlipQc.cpp;
    // SSR datum-break reactions → @c RealTimeEngineSsrBreaks.cpp; update /
    // constraint / AR / trace → @c RealTimeEngineEstimate.cpp.  New @c .cpp
    // seams need a manual @c CMakeLists.txt entry (no globbing).

    /**
     * @brief Per-epoch working set threaded through the @c processEpoch seams.
     *
     * Transport only: it owns nothing that outlives the epoch.  Every value
     * that must survive to the next epoch is a @c RealTimeEngine member
     * (@c prev_tow_, @c first_epoch_, @c ssr_phase_bias_applied_, …); putting
     * one of those here instead would silently cold-start it every epoch.
     *
     * The fields are the values that cross a seam boundary within one epoch:
     * the eleven locals that crossed a block boundary in the pre-cut driver,
     * plus @c pb_hold_handled.  A value that a single seam computes
     * and consumes itself stays local to that seam and does not belong here.
     */
    struct RtEpochContext {
        /// Raw epoch input, as handed to @c processEpoch.  Held by reference;
        /// the caller's vector outlives the context.  Read by the satellite-status
        /// trace, which surfaces BelowMask rows for sats @c ModelGeometry dropped
        /// and therefore needs the pre-filter set.
        const std::vector<RinexObservation>& epoch_obs;

        GnssTime    current_time{ 0, 0.0 };  ///< Epoch time (from @c epoch_obs.front()).
        std::string epoch_prefix;            ///< "[RT] " console tag.

        double dt{ 1.0 };                    ///< Time step to this epoch (s); 1.0 on the first.
        bool   gap_detected{ false };        ///< dt exceeded the data-gap tolerance (drives WLS re-seed).

        /// Pre-update epoch boundary + first-epoch flag captured for the aiding
        /// predict: @c prev_epoch_time_ and @c first_epoch_ are both clobbered
        /// before the predict site, but @c predictAided needs the (prev, current]
        /// integration interval and the first-epoch skip.
        GnssTime aiding_t_prev{ 0, 0.0 };
        bool     aiding_first{ false };

        /// The epoch's modeled observations, the input of every later seam.
        /// Mutated in place by the broadcast-grade drop and the BSSD re-gauge,
        /// so seams take it by reference and their order matters.
        std::vector<PppObservation> modeled_obs;

        /// Per-satellite drop attribution from @c computeModels, carried the full
        /// length of the epoch only so the satellite-status trace can report it.
        std::map<std::string, DropReason> drop_reasons;

        EpochMeasurement meas_update{};      ///< Committed measurement update (residual/DOP traces read it).
        bool update_applied{ false };        ///< A float update committed; gates AR and the trace "have_applied" flag.

        /// The "sat|code" keys @ref RealTimeEngine::reactToPbHold_ already handled
        /// this epoch.  A held key is judged afresh every epoch, so the
        /// set is per-epoch and belongs here, not in a member.  The three
        /// phase-bias reactions skip these keys, so exactly one seam reacts per
        /// (key, epoch).  Empty unless the producer armed @c network.emit_pb_hold,
        /// so every unheld run is unchanged.
        std::set<std::string> pb_hold_handled;
    };

    // --- Admission gates (RealTimeEngineAdmit.cpp) ------------------------
    // The only seams that can end the epoch.  Each returns false to mean
    // "processEpoch returns false".  Every early exit in them is a
    // `return false` carried over from the pre-cut driver, so the bodies are
    // verbatim and the only added code is the final `return true`.

    /// Real-time obs/wall-clock lag: backlog warn, obs↔SSR hold, skip-to-latest.
    /// Returns false when the epoch is deferred (held for SSR) or dropped (stale).
    bool admitBacklogAndSync(RtEpochContext& ctx);

    /// processing.max_duration_s data-time cutoff; latches @c max_duration_reached_.
    bool gateMaxDuration(RtEpochContext& ctx);

    /// Time step / gap detection; fills @c ctx.dt, @c ctx.gap_detected and the
    /// aiding capture, and advances @c prev_tow_ / @c prev_epoch_time_.
    /// Returns false on a non-advancing TOW (duplicate epoch).
    bool stepTimeAndDetectGap(RtEpochContext& ctx);

    /// Corrections-ready startup gate: defers all estimation until the modeled
    /// above-mask satellites with broadcast+SSR corrections meet the gate's
    /// global and per-constellation floors (all of them with
    /// require_all_above_mask), or until the optional timeout.  Returns false
    /// while deferring.
    bool gateCorrectionsReady(RtEpochContext& ctx);

    // --- Observability (RealTimeEngineSsrDiag.cpp) ------------------------
    void logSsrCoverage(RtEpochContext& ctx);    ///< Per-epoch SSR coverage summary line.
    void traceSsrConsumer(RtEpochContext& ctx);  ///< Consumer-side SSR-application CSV (L10).
    /// Harvest this epoch's per-(sat, raw phase signal) rows into
    /// @ref consumer_sat_rows_.  Runs at the processEpoch tail
    /// (post-update, post-AR) so the states and residuals it reads are the
    /// committed ones; no-op unless @ref consumer_sat_trace_.
    void traceConsumerSat_(RtEpochContext& ctx);
    /// Record what a reaction (an SSR datum-break reaction or the level check)
    /// did to one key, for this epoch's rows.  @p key is
    /// "<sat>|<signal>" for the phase/code-bias reactions and "<sat>|CLK" for the
    /// clock one; repeated actions on one key in one epoch are joined with ';'.
    /// No-op unless armed.
    void noteConsumerSatEvent_(const std::string& key, const char* action);

    // --- Ambiguity level re-validation, card ALC (RealTimeEngineEstimate.cpp) --
    /// Re-validate every dual-frequency satellite's float ambiguities against
    /// their own windowed code level and, in act mode
    /// (@c processing.amb_level_check_mode), refloat the ones the code refuses.
    /// Runs at the estimation tail, after the update / constraints / AR and
    /// before the CST harvest, so the trip reads this epoch's committed
    /// innovations and the refloat's variance re-inflation shows on the CST row.
    /// No-op unless @c processing.amb_level_check.
    void checkAmbiguityLevel_(RtEpochContext& ctx);
    /// Restart @p sat's level window because a reaction wrote its ambiguity this
    /// epoch.  No-op unless the check is armed.
    void noteAmbLevelDisturbed_(const std::string& sat);
    /// One INFO line with the level-check counters; silent unless armed.
    void logAmbLevelCheckSummary_(const char* tag);

    // --- Measurement-set conditioning (RealTimeEnginePrepare.cpp) ---------
    void dropBroadcastGrade(RtEpochContext& ctx); ///< Drop broadcast-grade sats from the precise solution.
    void seedClockFromWls(RtEpochContext& ctx);   ///< WLS clock seed; runs only with a receiver clock state (not BSSD; see regaugeBssd).
    void regaugeBssd(RtEpochContext& ctx);        ///< BSSD code/phase re-gauge; BSSD only, so mutually exclusive with the above.

    // --- Slip detection / repair (RealTimeEngineSlipQc.cpp) ---------------
    /// MW / TurboEdit / soft-LLI-bridge cycle-slip detection and repair, run
    /// after the KF predict and before the measurement update.  The largest
    /// single seam; about two thirds of it is reachable only with
    /// @c estimation.cycle_slip.turbo_edit armed.
    void detectAndRepairSlips(RtEpochContext& ctx);

    // --- SSR datum-break reactions (RealTimeEngineSsrBreaks.cpp) ----------
    // Five handlers for a producer that re-realizes its datum.  processEpoch
    // calls them in declaration order; the phase-bias variants declared between
    // them are dispatched from resetOnPhaseBiasDisc.  reactToGenerationChange runs
    // first: it applies the union of the per-signal reactions globally, because a
    // producer restart re-realizes every bias and level at once and re-seeds the
    // disc counters at 0, which the per-signal handlers would read as 0→0 (no
    // change).  All five read the SSR store and do nothing when it holds no SSR
    // content.
    void reactToGenerationChange(RtEpochContext& ctx);  ///< Generation-id change → global datum break (RTA-3).
    void logProducerStatus(RtEpochContext& ctx);        ///< Producer heartbeat → one-shot transition log (RTA-10).
    void resetOnPhaseBiasDisc(RtEpochContext& ctx);     ///< Phase-bias discontinuity / freshness flip → ambiguity reset.
    /// Carry twin of resetOnPhaseBiasDisc (card CC (A)): with
    /// processing.ssr_phase_bias_disc_carry armed, resetOnPhaseBiasDisc runs this
    /// instead of its reset body.  A declared step (disc-counter change) or an A5
    /// freshness flip translates the ambiguity by −Δ (PbDiscCarryLedger::react);
    /// refusals take the existing reset.  The ledger's record() is the update
    /// site: it runs per key after the reaction, with the value the measurement
    /// update will apply this epoch (the same gated read the model makes).
    void carryOnPhaseBiasDisc_(RtEpochContext& ctx);
    /// One INFO line with the carry counters; silent unless armed (card CC (A)).
    void logPbDiscCarrySummary_(const char* tag);
    /// Step twin of resetOnPhaseBiasDisc / carryOnPhaseBiasDisc_ (card PBT (A)):
    /// with processing.ssr_phase_bias_step_translate armed, resetOnPhaseBiasDisc
    /// runs this instead of both.  Every change in the applied phase bias
    /// (declared or not) beyond pb_step_translate_eps_m translates the ambiguity
    /// by −Δ (PbStepTranslateLedger::react).  Because it replaces the whole
    /// reaction, a declared step is translated once, not twice, even with the
    /// card-CC carry also armed.  Its record() is the update site, per key after
    /// the reaction.
    void translatePhaseBiasStep_(RtEpochContext& ctx);
    /// One INFO line with the translation counters; silent unless armed (card PBT (A)).
    void logPbStepTranslateSummary_(const char* tag);
    /// Reaction to a held phase bias, which resetOnPhaseBiasDisc runs
    /// ahead of all three reactions above.  It keys on the data, not on a
    /// consumer knob: a consumer cannot know the producer armed
    /// @c network.emit_pb_hold, it can only see
    /// @c SsrPhaseBiasEntry::hold_generation.  For a key whose entry carries a
    /// nonzero generation the emitted bias is constant within the generation, so
    /// a discontinuity-counter change carries no value step and is consumed and
    /// ignored, while a change of the generation itself re-floats the ambiguity
    /// through the existing reset path.  Every key it handles is recorded in
    /// @c RtEpochContext::pb_hold_handled and skipped by whichever of the three
    /// reactions runs next, so each key is still visited once per epoch.  With
    /// no held entries the set stays empty and all three are byte-identical.
    /// With @c processing.ssr_pb_hold_presence_flip armed, the downstream reaction
    /// skips only the claimed key's counter reaction and still answers its A5
    /// presence edge.
    void reactToPbHold_(RtEpochContext& ctx);
    /// One INFO line with the hold-reaction counters; silent unless a held entry
    /// was ever seen.
    void logPbHoldSummary_(const char* tag);
    /**
     * @brief Count one A5 presence edge if it belongs to a held key.
     *
     * Called by the three phase-bias reactions at an A5 edge.  Counts only with
     * @c processing.ssr_pb_hold_presence_flip armed (the off path never reaches a
     * claimed key's edge) and only for a key the hold ledger has recorded, so an
     * unheld key's edge is not attributed to the hold.
     *
     * @param key         The "<sat>|<signal>" key.
     * @param applied_now The bias is applied this epoch (an appearance) or not (a vanish).
     * @param suppressed  The declared-roster rule left the edge alone (card SID 5b).
     */
    void countPbHoldPresenceEdge_(const std::string& key, bool applied_now, bool suppressed);
    void reinflateOnCodeBiasDisc(RtEpochContext& ctx);  ///< Code-bias datum re-reference → rx code-bias re-inflation.
    void reinflateOnClockDisc(RtEpochContext& ctx);     ///< Clock datum re-reference → scoped ISB re-inflation (A2).
    /**
     * @brief Card CLV2: answer a declared code-level step (processing.ssr_level_step_inflate).
     *
     * For each satellite of this epoch whose declared step totals in the SSR store moved
     * since the engine last read them (the first reading only records), the change of
     * each of its code signals' prediction is -(clock step) + (that signal's code-bias
     * step), and sigma is the largest.  The engine's covariance then gains
     * (k sigma)^2 v v^T along the satellite's phase-blind direction v (ionosphere +1,
     * each phase ambiguity +alpha_f): the direction an undifferenced consumer absorbed the
     * old code level in, which the next epochs' code rows re-determine while the phase
     * rows stay as they were.  No-op when unarmed.
     */
    void reactToDeclaredLevelSteps_(RtEpochContext& ctx);
public:
    /**
     * @brief The inter-system biases to re-inflate on a clock re-reference (card OWD (O2)).
     *
     * A non-reference constellation's own step re-inflates its own ISB; a reference (GPS)
     * step re-inflates every allocated non-reference ISB, observed this epoch or not (the
     * ISB is a difference of two clock datums, and a constellation absent from the step
     * epoch would otherwise return stale by the whole step).  Returned as a set, so each
     * ISB is re-inflated at most once per epoch.
     *
     * @param reference_stepped A reference-constellation clock re-referenced this epoch.
     * @param stepped           The non-reference ISB names whose own constellation stepped.
     * @param allocated         The ISB states the consumer holds, keyed by system name
     *                          (@c StateManager::getSystemTimeOffsetStates()).
     * @return The ISB names to re-inflate.
     */
    static std::set<std::string> isbReinflationSet(bool reference_stepped,
                                                   const std::set<std::string>& stepped,
                                                   const std::map<std::string, size_t>& allocated);

    /**
     * @brief Log this consumer's state-manager re-inflation counters (card OWD (O2)).
     *
     * One line, "card OWD re-inflations (<tag>): code_bias raised=N kept=M | isb ... |
     * iono ... | iono_rate ... | sat_bias ... | sat_phase_bias ... | sat_clock ...", on the
     * @c rt_engine logger.  A nonzero kept count names a re-inflation that left a larger
     * current variance in place, where the rule before card OWD would have lowered it.
     *
     * @param tag Where the line comes from ("end of run", "closed loop", "end of replay").
     */
    void logReinflateSummary(const char* tag) const;
private:

    /// Read the producer's declared signal roster out of the SSR store once per
    /// epoch, before @c computeModels, and hand it to the latch (card SID
    /// phase 5).  The latch is decided inside that call at a constellation's first
    /// epoch, so a declaration that arrives later is too late, and the counters
    /// record that.  No-op on every wire that carries no declaration (every
    /// producer before card SID, every RTCM stream), which keeps a legacy
    /// consumer byte-identical.
    void refreshDeclaredRoster_();
    /// Classify one fresh-to-stale (or stale-to-fresh) freshness flip against the
    /// declaration and the family's presence on the wire, count it, and write an
    /// @c ARC event carrying its identity, up to a per-run cap (card SID phase 5,
    /// E3).
    ///
    /// @param key       "sat|signal" (phase) or "CB|sat|signal" (code).
    /// @param sat       Satellite id.
    /// @param signal    RINEX signal.
    /// @param cons      Constellation display name.
    /// @param domain    "phase" or "code".
    /// @param was_fresh The flag's previous value.
    /// @param now_fresh Its value now.
    /// @param on_wire   Whether an ungated read finds a record for this key.
    /// @param t         Epoch time.
    /// @return true when the flip is absent by declaration (the network declares
    ///         no family for this signal) and @c network.closed_loop.roster_latch
    ///         is armed: the one class whose response card SID phase 5b
    ///         suppresses.
    bool noteFreshnessFlip_(const std::string& key, const std::string& sat,
                            const std::string& signal, const std::string& cons,
                            const char* domain, bool was_fresh, bool now_fresh,
                            bool on_wire, const GnssTime& t);

    // --- Estimation tail (RealTimeEngineEstimate.cpp) ---------------------
    void runMeasurementUpdate(RtEpochContext& ctx);     ///< KF update with the outlier / pivot-handover rebuild loop.
    void applyZwdConstraint(RtEpochContext& ctx);       ///< Soft a-priori ZWD pseudo-observation.
    void applyNetworkIonoPrior(RtEpochContext& ctx);    ///< Network slant-iono BLUP prior (ionosphere-weighted).
    void resolveAmbiguities(RtEpochContext& ctx);       ///< Integer ambiguity resolution (MLAMBDA).
    void writeEpochTraces(RtEpochContext& ctx);         ///< Posterior extract, epoch log line, trace-logger sweep.

    // --- RT filter-state snapshot (RealTimeEngineStateSnap.cpp) -----------
    // The consumer-side warm start, the complement of RTB-7.  Armed by
    // realtime.state_snapshot_path; all three are no-ops (byte-identical) with
    // the path empty.  loadStateSnapshotStaged_ runs at the initialize() tail
    // and only stages the parsed file.  applyStagedStateSnapshot_ runs at the
    // top of the first processEpoch, the first point at which the fingerprint
    // can be checked against the fully armed engine (setSsrStore /
    // setConsumeNetworkIono run after initialize()) and the age against a real
    // epoch time.  A refused stage is discarded with a logged reason and the
    // cold start proceeds unchanged.  writeStateSnapshot_ runs at the
    // processEpoch tail on the configured data-time cadence.
    uint64_t rtSnapshotFingerprint_() const;            ///< FNV-1a over the state-meaning config facts.
    void loadStateSnapshotStaged_();                    ///< Parse + stage the snapshot file (initialize tail).
    void applyStagedStateSnapshot_(RtEpochContext& ctx);///< One-shot: age/fingerprint gate, layout rebuild, wholesale x/P + baselines restore.
    void writeStateSnapshot_(RtEpochContext& ctx);      ///< Cadence-gated atomic snapshot write.
    /// Harvest the live filter memory into @p out stamped at @p at (the shared
    /// body of writeStateSnapshot_ and the card-CC writeStateSnapshotTo).  False
    /// + @p why when the dynamic layout is unserializable (warned once per run).
    bool harvestStateSnapshot_(struct RtStateSnapshot& out, const GnssTime& at,
                               const std::string& epoch_prefix, std::string* why);

    // =====================================================================
    // initialize() seams (RealTimeEngineInit.cpp)
    // =====================================================================
    // Construction phase, run once before any frame is decoded.  The order is a
    // dependency chain: initProducts builds the product and RINEX parsers,
    // initStateManagerAndApriori reads the RINEX header for the a-priori seed,
    // and initProcessingComponents binds the models and the filter to the
    // parsers and the StateManager.  seedInitialState() must follow
    // initProcessingComponents(), which constructs the PppModel it calls
    // (PppModel::initializeState).  It applies the Earth-centred fallback after
    // initializeState and before the state-vector sync; a seed written to the
    // state vector earlier would be overwritten with ppp_state_'s zero position.
    // On a live stream (no RINEX APPROX POSITION) a zero position makes
    // computeModels reject every satellite (receiver-position check,
    // |r| < 1 km), so WLS never runs and the filter cannot bootstrap.

    void armSsrStoreOptions();          ///< Leap-second override + the opt-in process-wide SSR store policies.
    bool initProducts();                ///< Product/RINEX parsers, antenna seeding, product load + cache. False on cache failure.
    void initStateManagerAndApriori();  ///< StateManager + the coarse a-priori position seed.
    void initProcessingComponents();    ///< Models, filter, aiding, WLS, slip detectors + the "not wired into RT" warnings.
    void initAmbiguityResolver();       ///< MLAMBDA resolver setup.
    void seedInitialState();            ///< initializeState + the Earth-centred a-priori fallback.
    void initTraceLoggers();            ///< TraceLogger, the L10 SSR-consumer trace, slip traces.

    // =====================================================================
    // feedRtcmFrame() seams (RealTimeEngineIngest.cpp)
    // =====================================================================
    // The observation-buffering loop and the broadcast-ephemeris push stay in
    // the driver: both call anonymous-namespace helpers (isSystemActive,
    // refreshGlonassSlots) that are file-local to RealTimeEngine.cpp by design,
    // and moving those into a new internal header to relocate 51 lines costs
    // more than leaving the two blocks in place.

    /// Push decoded SSR records under the per-stream RTA-8(b) accept policy.
    /// @return the number of records the policy dropped (never pushed).
    std::size_t ingestSsrRecords(RtcmParser& parser, SsrAcceptPolicy accept);

    /// Per-frame SSR / ephemeris ingest counters + the obs↔SSR drain trigger.
    void logRtcmIngestCounters(RtcmParser& parser, std::size_t n_ssr_dropped);

    /// RTCM 1005/1006 reference-station ARP → filter a-priori seed (RTA-12(3) gated).
    void seedAprioriFromStationCoords(RtcmParser& parser);

    GnssConfig&  config_;
    LogManager&  log_manager_;

    // Processing components (same as Application, but owned here).
    std::unique_ptr<ProductParser>       product_parser_;
    std::unique_ptr<RinexParser>         rinex_parser_;
    std::unique_ptr<StateManager>        state_manager_;
    std::unique_ptr<PppModel>            ppp_model_;
    std::unique_ptr<MeasurementModel>    meas_model_;
    std::unique_ptr<KalmanFilter>        kalman_filter_;
    std::unique_ptr<AidingSourceManager> aiding_manager_;   //!< Aiding sources (IMU first client); shared predict path with Application.
    std::unique_ptr<WlsEstimator>        wls_estimator_;
    std::unique_ptr<MwSlipDetector>      mw_slip_detector_;
    /// TurboEdit MW+GF joint slip-repair resolver (estimation.cycle_slip.turbo_edit);
    /// nullptr unless the toggle is set.  Pairs the MW wide-lane integer with the
    /// geometry-free phase jump to repair dual-frequency slips per signal.
    std::unique_ptr<TurboEditResolver>   turbo_edit_resolver_;
    /// Soft-LLI gap-bridge orchestration (snapshot / hold / restore), shared
    /// with the post-processing CycleSlipManager so the two paths cannot
    /// diverge.  Inert unless estimation.cycle_slip.turbo_edit_bridge_lli.
    std::unique_ptr<SoftLliBridge>       soft_lli_;
    std::unique_ptr<AmbiguityResolver>   ambiguity_resolver_;
    std::unique_ptr<TraceLogger>         trace_logger_;
    /// Consumer-side SSR-application trace (network.trace_ssr_consumer), L10.  Opened in
    /// initialize() into the same "logs" dir as the producer traces; one row per modeled
    /// sat per processed epoch recording correction age / clk-applied / URA R-inflation /
    /// disc counters.  Never opened when the toggle is off (byte-identical).
    std::ofstream                        ssr_consumer_trace_;

    /// Assembles broadcast ephemeris from raw SBF nav subframes (GPSRawCA/
    /// GALRawINAV/BDSRaw); persistent across blocks, feeds LiveEphemerisStore.
    SubframeAssembler                    subframe_assembler_;

    PppState ppp_state_{};
    Eigen::Vector3d initial_position_{ 0.0, 0.0, 0.0 };

    /// Non-owning SSR store override; nullptr -> process-wide singleton.
    /// Set by setSsrStore() when consume_bias is true, so this is the
    /// bias-routed store that ssrStore() returns.
    const SsrCorrectionStore* ssr_store_override_{ nullptr };

    /// The clock-routed store (per setSsrStore's consume_clock), on which
    /// GnssCorrections tallies orbit-echo mismatches (card 7).  The
    /// ssr_store_override_ above is bias-routed (consume_bias), so the two differ
    /// under a clock-on/bias-off profile; the echo summary reads this one, since
    /// the bias-routed store never holds a split consumer's mismatches.
    const SsrCorrectionStore* clock_store_override_{ nullptr };

    /// Consume the network slant-iono supply as a tight per-sat prior (see
    /// @ref setConsumeNetworkIono).  Default false ⇒ processEpoch builds no iono
    /// pseudo-observation (byte-identical).
    bool consume_network_iono_{ false };

    /// Most recent epoch's AR result + whether AR ran (closed-loop metrics).
    AmbiguityResolution last_ar_result_{};
    bool last_ar_ran_{ false };

    // --- Per-satellite consumer readout, card CST (network.trace_consumer_sat) ---
    /// Armed by @ref setConsumerSatTrace.  Gates both the tail harvest and the
    /// per-key event recording (@ref noteConsumerSatEvent_), so an unarmed
    /// engine does no allocation and no map write anywhere on the epoch path.
    bool consumer_sat_trace_{ false };
    /// This epoch's harvested rows (cleared at the top of every processEpoch).
    std::vector<ConsumerSatRow> consumer_sat_rows_;
    /// This epoch's consumer reactions, "<sat>|<signal>" or "<sat>|CLK" -> the
    /// action name(s).  Filled by @ref noteConsumerSatEvent_ from the datum-break
    /// seams (which run before the measurement update) and from the level check
    /// (at the tail, before the harvest), and drained by @ref traceConsumerSat_;
    /// cleared with the row buffer.
    std::map<std::string, std::string> consumer_sat_events_;

    /// Card CLV2: the declared code-level step totals last read per satellite
    /// (@ref reactToDeclaredLevelSteps_), and its counters.
    std::map<std::string, SsrCorrectionStore::DeclaredLevelSteps> level_steps_seen_;
    long long level_step_events_{ 0 };     ///< (sat, epoch) declared code steps above the dead band
    long long level_step_inflated_{ 0 };   ///< …answered by an inflation
    long long level_step_refused_{ 0 };    ///< …refused: no ionosphere state, a frozen row, or no ambiguity
    double    level_step_sigma_sum_{ 0.0 };///< sum of the answered sigmas (m)
    double    level_step_sigma_max_{ 0.0 };///< largest answered sigma (m)

    /// When true, AR ignores the MW WL feed and runs pure MLAMBDA on the KF float
    /// (closed-loop network-phase-bias validation; see setArIgnoreMwWl).
    bool ar_ignore_mw_wl_{ false };

    uint64_t  epoch_count_{ 0 };
    bool      first_epoch_{ true };

    // processing.max_duration_s data-time cutoff (0 = unlimited).  The window
    // is anchored on the first fully processed epoch (deferred warmup epochs
    // never reach the anchor site), so the budget covers real estimation time.
    bool      max_duration_anchored_{ false }; ///< True once the cutoff window has been anchored.
    double    max_duration_anchor_s_{ 0.0 };   ///< Continuous GPS seconds of the first processed epoch.
    bool      max_duration_reached_{ false };  ///< Latched once the elapsed data-time span hits the limit.

    // Corrections-ready startup gate (estimation.defer_until_corrections_ready).
    // While armed and unsatisfied, processEpoch defers all estimation (no
    // predict/update) until the gate's floors on broadcast+SSR above-mask sats
    // are met or its optional timeout expires (see gateCorrectionsReady).
    bool      estimation_started_{ false }; ///< Gate passed (ready or timed out); estimation has begun (latched).
    uint64_t  warmup_epochs_{ 0 };          ///< Count of deferred epochs (throttled progress logging).
    GnssTime  warmup_start_time_{ 0, 0.0 }; ///< First deferred epoch time (optional timeout fallback).

    bool      apriori_seeded_from_pvt_{ false };  ///< Logged once when the a-priori is taken from PVTGeodetic.
    bool      apriori_seeded_from_station_{ false }; ///< Logged once when the a-priori is taken from an RTCM 1005/1006 station ARP.
    bool      arp_blunder_warned_{ false };          ///< Warned once that a decoded 1005/1006 ARP failed the realtime.arp_blunder_gate_m cross-check against the existing a-priori or the configured reference position (RTA-12(3)).
    bool      warned_ssr_cb_no_ifb_{ false };        ///< Warned once that network code biases are consumed while estimate_ifb is off (no rx code-bias state to absorb a datum re-reference; A5).
    // Per-(sat|signal) record of whether the SSR phase bias was applied (fresh
    // within the staleness gate) at the previous epoch (A5, P1-4).  Maintained only
    // when the bias staleness gate (enable_ssr_bias_max_age) is on; a fresh↔stale
    // flip is a full-bias phase step with no producer disc-counter change, so the
    // ambiguity is reset on the flip.
    std::map<std::string, bool> ssr_phase_bias_applied_;
    // Code analog of the map above (A5): per-("CB|sat|signal") record of whether the
    // SSR code bias was applied (present + fresh within the staleness gate) at the
    // previous epoch.  Maintained only when the gate is on.  A fresh↔absent flip,
    // including a signal that disappears from a wholesale-replacing producer push
    // (which carries no disc counter), steps the applied code correction by the
    // full bias, so the affected constellation's receiver code-bias states are
    // re-inflated on the flip.
    std::map<std::string, bool> ssr_code_bias_applied_;
    /// The producer's declared signal roster as this consumer last read it out
    /// of the SSR store (@ref refreshDeclaredRoster_; card SID phase 5).
    /// @c present is false on every wire that carries none, which leaves every
    /// use site inert and the engine byte-identical.
    NetworkSignalDeclaration ssr_declared_roster_;
    bool declared_roster_logged_{ false };   ///< one-shot arming / inert line.
    RosterConsumerCounters roster_consumer_; ///< The consumer census (card SID phase 5).
    /// Last observed network solution-generation id from the SSR store (RTA-3;
    /// 0 = never seen).  A change of a present id means the producer restarted
    /// and re-realized its whole datum: every phase-bias lump and code-bias level
    /// at once, with no per-signal disc-counter continuity (a restarted
    /// producer's counters re-seed at 0, so a warm consumer holding 0 sees 0→0,
    /// "no change").  The engine reacts with a global datum break: reset all
    /// ambiguities and re-inflate every receiver code-bias state.  An absent id
    /// (RTCM streams, pre-RTA-3 products) never triggers it, so those runs are
    /// byte-identical.
    uint32_t  last_ssr_generation_id_{ 0 };
    // --- RT filter-state snapshot (RealTimeEngineStateSnap.cpp) -----------
    /// Parsed-but-unapplied snapshot staged by loadStateSnapshotStaged_; applied
    /// or discarded (one-shot) at the top of the first processEpoch.  Never set
    /// unless realtime.state_snapshot_path is non-empty.
    std::unique_ptr<struct RtStateSnapshot> staged_rt_snap_;
    double    rt_snap_last_write_s_{ -1.0 };   ///< Continuous GPS seconds of the last snapshot write (-1 = never).
    bool      rt_snap_layout_warned_{ false }; ///< One-shot warn for an unserializable state layout (unknown dynamic index).
    RtSnapshotRestoreStatus rt_snap_restore_;  ///< Outcome of the one-shot staged resume (Restored, Discarded or NotStaged; card CC (B)).
    /// The epoch the live filter memory is "as of" (card CC (B)): set at the
    /// processEpoch tail, just before the cadence-gated snapshot write, so
    /// writeStateSnapshotTo stamps (and checks) that epoch.
    GnssTime  last_processed_epoch_{ 0, 0.0 };
    bool      has_processed_epoch_{ false };
    /// Per-(sat|signal) applied-bias ledger + carry counters (card CC (A)).  Only
    /// touched by carryOnPhaseBiasDisc_ (the knob-on twin of the reset reaction).
    /// Not part of the RT snapshot, whose v1 format is unchanged: the first event
    /// per key after a restore takes the reset (see RtStateSnapshot.hpp).
    PbDiscCarryLedger pb_disc_carry_;
    /// Per-(sat|signal) applied-bias ledger + translation counters (card PBT (A)).
    /// Only touched by translatePhaseBiasStep_ (the knob-on replacement for the
    /// reset / carry reactions).  Not part of the RT snapshot, whose v1 format is
    /// unchanged: the first cell per key after a restore has no (k−1, k) pair, so
    /// a declared one takes the reset and an undeclared one is a no-op.
    PbStepTranslateLedger pb_step_translate_;
    /// Per-(sat|signal) hold-generation memory + the ignore-vs-refloat decision +
    /// counters.  Only touched by reactToPbHold_, and only for entries
    /// carrying a nonzero generation, so an unheld product never reaches it.  Not
    /// part of the RT snapshot, whose v1 format is unchanged: a restored
    /// consumer's first held entry per key is a first sighting, which re-seats the
    /// memory without a reset, and the producer's own hold state cold-starts on
    /// its own resume.
    PbHoldLedger pb_hold_;
    /// Presence edges of held keys.  Counted on every consumer of a held
    /// product; the reaction to them is gated by processing.ssr_pb_hold_presence_flip.
    PbHoldPresenceCounters pb_hold_presence_;

    // --- Consumer ambiguity level re-validation --------------------
    /// The windowed code levels + the two tests + the counters.  Configured once
    /// on the first armed epoch (@c amb_level_configured_); untouched, and never
    /// sampled, with @c processing.amb_level_check off.
    AmbLevelCheck amb_level_check_;
    bool          amb_level_configured_{ false };   ///< The three knobs have been resolved onto @ref amb_level_check_.
    /// This epoch's AMB_LEVEL rows; cleared at the top of @c processEpoch, like
    /// the CST buffers, and drained by the closed-loop harness afterwards.
    std::vector<AmbLevelEvent> amb_level_events_;
    /// This epoch's full-window evaluations, drained with the events.
    uint64_t amb_level_epoch_checks_{ 0 };
    /// Generations of the two selected ambiguity signals at the last ALC sample.
    /// StateManager assigns unique IDs across births, resets and translations,
    /// so this also detects a signal-pair change or removal/rebirth of a key.
    /// Kept across deferred epochs and missing observations; never snapshotted.
    std::map<std::string, std::pair<uint64_t, uint64_t>> amb_level_generations_;
    /// Last observed network-producer epoch-status state from the SSR store's
    /// heartbeat surface (RTA-10; nullopt = never seen).  Edge trigger for the
    /// one-shot transition log (ok→withheld_sigma0 etc.): the first sighting
    /// logs once, then only changes log.  Absent heartbeats (live RTCM streams,
    /// pre-RTA-10 products, network.emit_epoch_status_record off) never log, so
    /// the console output for such content is byte-identical.  Gating
    /// measurement usage on producer status is out of scope here (RTF-9).
    std::optional<NetworkProducerStatus::State> last_producer_status_;
    double    prev_tow_{ 0.0 };
    GnssTime  prev_epoch_time_{ 0, 0.0 };
    uint64_t  backlog_warn_ctr_{ 0 };          ///< Throttle counter for the obs-backlog warning (every Nth lagging epoch).

    /// Persistent RTCM parser (RTB-1(a)): one per engine, reused for every
    /// frame from every stream (primary / corrections / ephemeris share the
    /// GPS timeline).  Persistence keeps the parser's stream-time tracker
    /// continuous, so mod-week epoch fields are promoted by nearest continuation
    /// to the last decoded epoch instead of re-anchoring on the host clock every
    /// frame.  clear() runs at
    /// the top of feedRtcmFrame (outputs are per-frame; the tracker survives).
    RtcmParser                      rtcm_parser_;

    /// MSM lock-time history for the LLI 0x1 derivation at epoch assembly
    /// (RTB-3; armed by realtime.msm_lli_from_locktime).  Station key "": this
    /// engine serves one rover; the multi-station front-end
    /// (NetworkStreamFrontEnd) keys its own tracker per station.
    LockTimeTracker                 lock_time_tracker_;

    // Observation buffer for epoch assembly from streaming frames.
    GnssTime                        pending_epoch_time_{ 0, 0.0 };
    std::vector<RinexObservation>   pending_obs_;

    // Receipt stamps for receive→process latency accounting (RTB-1(b)).
    // pending_* tracks the first frame that contributed observations to the
    // epoch being assembled; processing_* is copied for the epoch handed to
    // processEpoch (valid only on the RTCM assembly path; direct feeds and
    // held-epoch re-drains have no stamp).  Diagnostics only.
    std::chrono::steady_clock::time_point pending_epoch_receipt_{};
    bool                            pending_epoch_receipt_valid_{ false };
    std::chrono::steady_clock::time_point processing_epoch_receipt_{};
    bool                            processing_epoch_receipt_valid_{ false };

    // --- obs↔SSR sync (processing.enable_obs_ssr_sync) ---
    // A complete obs epoch deferred because it ran ahead of the SSR coverage
    // horizon (freshest SSR content epoch + ssr_max_age).  Keep-latest: a newer
    // held epoch overwrites the older one (skip-to-latest semantics).  Drained by
    // drainHeldObs() when an SSR push advances the horizon.  Empty on every
    // non-sync run, so the feature is fully inert unless enabled.
    std::vector<RinexObservation>   held_obs_;
    GnssTime  held_epoch_time_{ 0, 0.0 };       ///< Epoch time of held_obs_ (diagnostics).
    double    sync_hold_since_s_{ 0.0 };        ///< Wall-clock GPS sec the current hold run began (0 = not holding).
    bool      sync_outage_{ false };            ///< Latched once a hold exceeds the max-hold budget; cleared when SSR re-covers obs (degraded passthrough while set).
    bool      processing_synced_epoch_{ false };///< True while draining a held epoch; bypasses skip-to-latest (the held epoch is the intended SSR-aligned target).
    bool      draining_final_{ false };         ///< True during flush(); bypasses both the obs↔SSR hold and skip-to-latest so the final epoch is never lost.
    uint64_t  sync_hold_warn_ctr_{ 0 };         ///< Throttle for the obs↔SSR hold debug line.
};

} // namespace GnssProcessor
