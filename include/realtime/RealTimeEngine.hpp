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
 *   3. Call processRtcmFrame() each time a complete frame is available.
 *   4. Call processEpoch() when a full epoch of observations is buffered.
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
#include <Eigen/Dense>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
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
     * @brief RTA-8(b): per-stream SSR ingest accept policy.
     *
     * Applied on the SSR push path in @ref feedRtcmFrame, per FRAME — the
     * driver (@c Application::runRealTime) passes the policy of the stream a
     * frame came from, so different streams get different filters.  Configured
     * per corrections/ephemeris stream via the @c corrections_accept key
     * (@c realtime.corrections.corrections_accept /
     * @c realtime.ephemeris.corrections_accept).
     *
     * @c OrbitOnly exists for the "orbits from the external RTS stream,
     * clocks/biases from OUR network product" wiring: without it, the RTS
     * stream's 1060/1066 combined records would land clock content in the
     * singleton and shadow the network clock wherever the singleton is
     * consulted.  Under @c OrbitOnly the stream's orbit and URA records push
     * as today, standalone clock / code-bias / phase-bias / high-rate-clock
     * records are DROPPED, and combined orbit+clock records are DECOMPOSED
     * into an orbit-only push (the store's combined-wins merge logic is never
     * touched — the combined record simply never enters the store).
     */
    enum class SsrAcceptPolicy {
        All,       ///< Push every decoded SSR record (default; byte-identical legacy path).
        OrbitOnly  ///< Push orbit + URA; drop clock/bias content; decompose combined → orbit.
    };

    /// RTA-8(b): kinds of decoded SSR records the push path routes (mirrors the
    /// RtcmParser getter set / SsrCorrectionStore push API one-to-one).
    enum class SsrRecordKind {
        Orbit, Clock, Combined, CodeBias, PhaseBias, Ura, HighRateClock
    };

    /// RTA-8(b): what the push site should do with one record under a policy.
    enum class SsrPushAction {
        Push,           ///< Push the record unchanged.
        Drop,           ///< Do not push.
        DecomposeOrbit  ///< Combined under OrbitOnly: push only the orbit part (see combinedOrbitPart).
    };

    /**
     * @brief RTA-8(b): the record-in → push-decision-out ingest filter.
     *
     * Pure function (factored out of @ref feedRtcmFrame so the policy matrix is
     * unit-testable).  @c All → @c Push for every kind.  @c OrbitOnly →
     * @c Push for Orbit/Ura, @c DecomposeOrbit for Combined, @c Drop for
     * Clock/CodeBias/PhaseBias/HighRateClock (high-rate clock IS clock
     * content — an additive delta on the polynomial — so it is dropped with
     * the rest of the clock family).
     */
    static SsrPushAction ssrPushAction(SsrAcceptPolicy policy, SsrRecordKind kind);

    /**
     * @brief RTA-8(b): extract the orbit-only part of a combined orbit+clock record.
     *
     * Copies sat / IODE / RAC delta + rate / received & content epochs /
     * IOD-SSR into an @c SsrOrbitCorrection and discards c0/c1/c2 — pushing the
     * result via @c SsrCorrectionStore::pushOrbit yields a record whose
     * @c getOrbitClock merge reports @c has_orbit=true, @c has_clock=false.
     * The decomposition happens at the PUSH SITE; the store's combined-wins
     * preference is deliberately untouched.
     */
    static SsrOrbitCorrection combinedOrbitPart(const SsrCombinedCorrection& rec);

    /**
     * @brief RTA-8(b): parse a @c corrections_accept config string.
     *
     * @c "all" (or empty — the unset default) → @c All; @c "orbit_only" →
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
     * @param accept RTA-8(b): SSR ingest policy for the stream this frame came
     *        from (default @c All — every pre-existing caller byte-identical).
     *        Observations / broadcast nav / station coords are never filtered;
     *        the policy touches only the SSR push loop.
     * @param receipt RTB-1(b): steady-clock receipt stamp from the framer
     *        boundary (@c RtcmStreamReader::lastFrameReceipt) for sub-second
     *        receive→process latency accounting.  The stamp of the FIRST
     *        frame contributing observations to a buffered epoch is carried
     *        to that epoch's processEpoch and surfaced on the obs-backlog
     *        diagnostics; it never gates control flow.  Defaults to "now"
     *        (a caller without a framer stamp loses only framer→feed skew).
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
     * stream — SBF carries only the rover observations and nav.
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
     * is unaffected.  The RT-IMU end-to-end path is wired but not yet validated
     * against live IMU data (the rooftop AsteRx-U is GNSS-only); see
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
     * Steps: WLS seeding -> KF predict -> cycle-slip detection ->
     * KF update -> optional relinearization -> state extraction.
     *
     * @param epoch_obs Observations for this epoch.
     * @return true if the epoch was processed successfully.
     */
    bool processEpoch(const std::vector<RinexObservation>& epoch_obs);

    /**
     * @brief RTA-8(a): where the SSR ORBIT overlay reads from when a store is
     *        injected via @ref setSsrStore.
     *
     * The pre-split @ref setSsrStore routed the orbit overlay together with the
     * clock (@c FollowClock — kept as the default so the closed-loop harness
     * call site behaves exactly as it always has).  The standalone
     * network-consumer wiring instead selects @c External (default of
     * @c processing.network_consumer.orbit_source): the orbit overlay stays on
     * the process-wide singleton — fed by the external RTS corrections stream,
     * ideally ingest-filtered to @c orbit_only (RTA-8(b)) — while clocks and
     * biases come from the injected network-product store.  @c Network is for
     * a future product that carries its own orbit corrections.
     */
    enum class SsrOrbitSource {
        FollowClock,  ///< Legacy coupling: orbit overlay routed with the clock (per consume_clock).
        External,     ///< Orbit overlay reads the SINGLETON (external RTS stream); clocks per consume_clock.
        Network       ///< Orbit overlay reads the INJECTED store (orbit-bearing network product).
    };

    /**
     * @brief Inject a non-owning SSR-correction store override (DI seam).
     *
     * Repoints the engine's SSR consumption — satellite clock (via the PppModel
     * geometry/corrections), satellite code/phase bias (via the MeasurementModel),
     * the phase-bias-discontinuity → ambiguity-reset hook, and the coverage
     * diagnostics — at @p store instead of the process-wide singleton.  The
     * PPP-RTK network closed-loop harness drives a held-out station through this
     * engine reading the producer's OWNED store (see ClosedLoopConfig).  Call
     * AFTER @ref initialize (so the models exist).  @c nullptr restores the
     * singleton.  RTCM/SBF PUSHES are unaffected — they always target the
     * singleton — so this is consume-side only and a no-op for live streaming.
     *
     * @param store Non-owning store pointer (must outlive the engine) or nullptr.
     * @param consume_clock  Route the SSR CLOCK overlay (PppModel geometry +
     *        corrections) at @p store.  Set false to keep precise SP3/CLK for the
     *        clock while still consuming the satellite phase bias — the cleanest
     *        integer-recovery test, isolating the phase-bias product from a
     *        marginal network clock product.
     * @param consume_bias   Route the SSR code/phase BIAS (MeasurementModel) and
     *        the phase-bias-discontinuity reset hook at @p store.
     * @param orbit_source   RTA-8(a): orbit-overlay routing.  Default
     *        @c FollowClock reproduces the pre-split coupling (orbit routed with
     *        the clock per @p consume_clock) so every existing caller — the
     *        closed-loop harness in particular — is byte-identical.  See
     *        @ref SsrOrbitSource.
     */
    void setSsrStore(const SsrCorrectionStore* store,
                     bool consume_clock = true, bool consume_bias = true,
                     SsrOrbitSource orbit_source = SsrOrbitSource::FollowClock);

    /// Result of the most recent epoch's ambiguity-resolution attempt (closed-loop
    /// harness metric).  @ref lastArRan reports whether AR was actually invoked
    /// this epoch (resolver present AND a float update committed).
    const AmbiguityResolution& lastArResult() const { return last_ar_result_; }
    bool lastArRan() const { return last_ar_ran_; }

    /// Suppress the MW wide-lane feed into ambiguity resolution, so AR runs PURE
    /// MLAMBDA on the Kalman float.  The network closed-loop validation uses this
    /// with full LAMBDA: the KF float carries the consumed network satellite phase
    /// bias, whereas the MW detector's WL integers are in the (separate) .BIA
    /// frame — feeding them would re-inject a conflicting WL datum.  Default false
    /// (live/normal AR uses the MW WL to halve the search).  JSON exposure (RTA-7):
    /// @c processing.ar_ignore_mw_wl (standalone) and the
    /// @c processing.network_consumer profile (see
    /// @c GnssConfig::networkConsumerArIgnoreMwWl) — wired in
    /// @c Application::runRealTime.
    void setArIgnoreMwWl(bool ignore) { ar_ignore_mw_wl_ = ignore; }

    /// @brief Arm consumption of the network's UNDIFFERENCED per-satellite slant
    ///        ionosphere as an ionosphere-weighted (BLUP) prior.
    ///
    /// When on, @ref processEpoch (after the measurement update, before AR)
    /// queries the injected SSR store's slant-iono channel
    /// (@c SsrCorrectionStore::getSlantIono) for each tracked satellite and pulls
    /// the corresponding per-satellite slant-iono state toward the supplied value
    /// via a common-mode-free pseudo-observation — replacing the loose
    /// @c ionosphere_q random walk with a strong (ionosphere-corrected) user model.
    /// Reads the store routed by @ref setSsrStore's @c consume_bias (the
    /// closed-loop consumer always routes bias, so this is the OWNED network
    /// store).  Default false ⇒ no pseudo-observation is built and the epoch is
    /// byte-identical.  Set by the closed-loop harness from
    /// @c closed_loop.consume_network_iono.
    void setConsumeNetworkIono(bool on) { consume_network_iono_ = on; }

    /**
     * @brief RTA-6: product-arrival wake for the obs↔SSR hold buffer.
     *
     * The hold buffer (processing.enable_obs_ssr_sync) drains only when an RTCM
     * SSR push advances the coverage horizon (@c feedRtcmFrame).  Network
     * products injected through @ref setSsrStore arrive OUTSIDE that path — the
     * @c runRealTime driver polls a @c NetworkProductTail and pushes epochs into
     * the injected store directly — so the driver calls this after delivering
     * >= 1 new product epoch to run the SAME drain check (@c drainHeldObs).
     * No-op when nothing is held (every non-sync run), exactly like the RTCM
     * trigger site.
     */
    void notifyProductsAdvanced() { drainHeldObs(); }

    /// Get the current receiver position (ECEF, meters).
    Eigen::Vector3d getPosition() const;

    /// Get the current receiver clock bias (meters).
    double getClockBias() const;

    /// Get the current ZWD estimate (meters).
    double getTroposphereZwd() const;

    /// Get the full PPP state.
    const PppState& getState() const { return ppp_state_; }

    /// Number of epochs processed so far.
    uint64_t epochCount() const { return epoch_count_; }

    /// True once the processing.max_duration_s data-time budget has been
    /// reached (latched).  The real-time poll loop checks this to end the
    /// session cleanly, mirroring how post-processing breaks its epoch loop.
    /// Always false when max_duration_s is 0 (the default, unlimited).
    bool maxDurationReached() const { return max_duration_reached_; }

private:
    /// Convert GPS-scale time to UTC-scale time.
    static GnssTime gpsToUtcTime(const GnssTime& gps_time);

    /// Resolve the active SSR store: the injected override when set, else the
    /// process-wide singleton.  Defined in the .cpp (full type visible there).
    const SsrCorrectionStore& ssrStore() const;

    /// @brief Re-attempt the held obs↔SSR-sync epoch after the SSR stream advances.
    ///
    /// Called from @c feedRtcmFrame after an SSR push.  Moves @c held_obs_ back
    /// through @c processEpoch with the sync gate re-evaluated: if the freshest
    /// SSR now covers the held epoch it is processed, otherwise it is re-held.
    /// No-op when nothing is held (the common case and all non-sync runs).
    void drainHeldObs();

    /// @brief RTA-13: log the one-line SSR bias attribute-mismatch summary.
    ///
    /// Reads @c ssrStore().biasAttributeMismatchCounts() and logs one INFO line
    /// with per-domain totals + the top offending (sat|signal|domain) keys.
    /// Silent when no mismatch was ever counted (every clean run).  Called
    /// periodically from @c processEpoch (every 300 processed epochs) and once
    /// from @c flush() (end-of-run).
    void logBiasAttrMismatchSummary(const char* tag);

    /// @brief Card 7: log the one-line orbit/clock coherence-echo mismatch summary.
    ///
    /// Reads @c ssrStore().orbitEchoMismatchCounts() and logs one INFO line with
    /// the total + the top offending (sat|reason) keys (reason ∈ noorbit / iod /
    /// skew) and whether the withhold budget is armed.  Silent when no mismatch
    /// was ever counted (every clean and every pre-Card-7 run).  Called from the
    /// same two hooks as the RTA-13 summary: periodically from @c processEpoch
    /// and once from @c flush() (end-of-run).
    void logOrbitEchoMismatchSummary(const char* tag);

    // =====================================================================
    // processEpoch seams
    // =====================================================================
    //
    // @c processEpoch was a single ~1,440-line epoch-loop body accreting SSR
    // sync, ephemeris reaction, measurement build, filter update, AR and emit —
    // structurally what @c Application::runNetwork looked like before its
    // INC-0..12 decomposition.  It is now a thin ordered driver over the
    // single-concern seams below, cut BEFORE the RT service queue (RTF-4..9,
    // 305-station scale) lands more of the same.
    //
    // **Seam contract** (adapted from the network one; see CLAUDE.md "Network
    // producer — where new code goes"):
    //
    //  1. Seams are PRIVATE MEMBER functions, not free functions over a context
    //     struct.  The network needed @c NetworkRunContext because @c runNetwork
    //     kept its state in function locals; every persistent value here is
    //     already a @c RealTimeEngine member, so a member function reaches
    //     @c state_manager_ / @c config_ / @c ssrStore() with no plumbing.
    //  2. Each seam opens with an ALIAS PREAMBLE reconstructing the driver's
    //     local names (@c log, @c epoch_prefix, @c modeled_obs, …) so the body
    //     reads exactly as it did in the driver.  This is what makes the
    //     relocation mechanically verifiable — see @c scripts/rt_seam_verify.py,
    //     which reconstitutes each body and asserts a character-exact match
    //     against the frozen pre-cut source.
    //  3. A seam owns NO state.  Cross-seam values travel in @ref RtEpochContext,
    //     which lives on the stack for exactly one @c processEpoch call.
    //  4. @c void seams cannot abort the epoch.  @c bool seams return
    //     false == "epoch is done, return false to the caller"; the driver reads
    //     @c if (!seam(ctx)) return false.  Only the four admission gates are
    //     bool — every seam from the SSR coverage census onward is
    //     unconditionally straight-line, which is why the cut is safe.
    //
    // **Where new code goes** (anti-regrowth): a new per-epoch QC/estimation step
    // goes in the seam matching its concern or a new one, NOT inline in the
    // driver.  Admission/deferral → @c RealTimeEngineAdmit.cpp; observability →
    // @c RealTimeEngineSsrDiag.cpp; measurement-set conditioning →
    // @c RealTimeEnginePrepare.cpp; slip/repair → @c RealTimeEngineSlipQc.cpp;
    // SSR datum-break reactions → @c RealTimeEngineSsrBreaks.cpp; update /
    // constraint / AR / trace → @c RealTimeEngineEstimate.cpp.  New @c .cpp
    // seams need a manual @c CMakeLists.txt entry (no globbing).

    /**
     * @brief Per-epoch working set threaded through the @c processEpoch seams.
     *
     * Transport only — it owns nothing that outlives the epoch.  Every value
     * that must survive to the next epoch is a @c RealTimeEngine member
     * (@c prev_tow_, @c first_epoch_, @c ssr_phase_bias_applied_, …); putting
     * one of those here instead would silently cold-start it every epoch.
     *
     * These eleven fields are precisely the locals that crossed a block
     * boundary in the pre-cut driver.  Anything a single seam computes and
     * consumes itself stays local to that seam and does NOT belong here.
     */
    struct RtEpochContext {
        /// Raw epoch input, as handed to @c processEpoch.  Held by reference —
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

        /// The epoch's modeled observations — the spine of the whole loop.
        /// MUTATED in place by the broadcast-grade drop and the BSSD re-gauge,
        /// so seams take it by reference and order matters.
        std::vector<PppObservation> modeled_obs;

        /// Per-satellite drop attribution from @c computeModels, carried the full
        /// length of the epoch purely so the satellite-status trace can report it.
        std::map<std::string, DropReason> drop_reasons;

        EpochMeasurement meas_update{};      ///< Committed measurement update (residual/DOP traces read it).
        bool update_applied{ false };        ///< A float update committed — gates AR and the trace "have_applied" flag.
    };

    // --- Admission gates (RealTimeEngineAdmit.cpp) ------------------------
    // The only seams that can abort the epoch.  Each returns false to mean
    // "processEpoch returns false"; every early exit they contain was already a
    // `return false` in the pre-cut driver, so the bodies are verbatim and the
    // transformation is confined to appending `return true`.

    /// Real-time obs/wall-clock lag: backlog warn, obs↔SSR hold, skip-to-latest.
    /// Returns false when the epoch is deferred (held for SSR) or dropped (stale).
    bool admitBacklogAndSync(RtEpochContext& ctx);

    /// processing.max_duration_s data-time cutoff; latches @c max_duration_reached_.
    bool gateMaxDuration(RtEpochContext& ctx);

    /// Time step / gap detection; fills @c ctx.dt, @c ctx.gap_detected and the
    /// aiding capture, and advances @c prev_tow_ / @c prev_epoch_time_.
    /// Returns false on a non-advancing TOW (duplicate epoch).
    bool stepTimeAndDetectGap(RtEpochContext& ctx);

    /// Corrections-ready startup gate: defers ALL estimation until every modeled
    /// above-mask satellite is broadcast+SSR.  Returns false while deferring.
    bool gateCorrectionsReady(RtEpochContext& ctx);

    // --- Observability (RealTimeEngineSsrDiag.cpp) ------------------------
    void logSsrCoverage(RtEpochContext& ctx);    ///< Per-epoch SSR coverage summary line.
    void traceSsrConsumer(RtEpochContext& ctx);  ///< L10 consumer-side SSR-application CSV.

    // --- Measurement-set conditioning (RealTimeEnginePrepare.cpp) ---------
    void dropBroadcastGrade(RtEpochContext& ctx); ///< Drop broadcast-grade sats from the precise solution.
    void seedClockFromWls(RtEpochContext& ctx);   ///< WLS clock seed (non-BSSD only — see regaugeBssd).
    void regaugeBssd(RtEpochContext& ctx);        ///< BSSD code/phase re-gauge (BSSD only — mutually exclusive with the above).

    // --- Slip detection / repair (RealTimeEngineSlipQc.cpp) ---------------
    /// MW / TurboEdit / soft-LLI-bridge cycle-slip detection and repair, run
    /// pre-estimation.  The single largest seam; ~two thirds of it is reachable
    /// only with @c estimation.cycle_slip.turbo_edit armed.
    void detectAndRepairSlips(RtEpochContext& ctx);

    // --- SSR datum-break reactions (RealTimeEngineSsrBreaks.cpp) ----------
    // Five handlers for "the producer moved its datum under us".  All five are
    // no-ops without SSR content, which is why live RTCM streams are unaffected.
    void reactToGenerationChange(RtEpochContext& ctx);  ///< RTA-3 generation-id change → GLOBAL datum break.
    void logProducerStatus(RtEpochContext& ctx);        ///< RTA-10 producer heartbeat → one-shot transition log.
    void resetOnPhaseBiasDisc(RtEpochContext& ctx);     ///< Phase-bias discontinuity / freshness flip → ambiguity reset.
    void reinflateOnCodeBiasDisc(RtEpochContext& ctx);  ///< Code-bias datum re-reference → rx code-bias re-inflation.
    void reinflateOnClockDisc(RtEpochContext& ctx);     ///< Clock datum re-reference → scoped ISB re-inflation (A2).

    // --- Estimation tail (RealTimeEngineEstimate.cpp) ---------------------
    void runMeasurementUpdate(RtEpochContext& ctx);     ///< KF update with the outlier / pivot-handover rebuild loop.
    void applyZwdConstraint(RtEpochContext& ctx);       ///< Soft a-priori ZWD pseudo-observation.
    void applyNetworkIonoPrior(RtEpochContext& ctx);    ///< Network slant-iono BLUP prior (ionosphere-weighted).
    void resolveAmbiguities(RtEpochContext& ctx);       ///< Integer ambiguity resolution (MLAMBDA).
    void writeEpochTraces(RtEpochContext& ctx);         ///< Posterior extract, epoch log line, trace-logger sweep.

    // --- RT filter-state snapshot (RealTimeEngineStateSnap.cpp) -----------
    // The consumer-side warm start — the RTB-7 complement.  Armed by
    // realtime.state_snapshot_path; all three no-op (byte-identical) with the
    // path empty.  loadStateSnapshotStaged_ runs at the initialize() tail and
    // only STAGES the parsed file; applyStagedStateSnapshot_ runs at the top of
    // the first processEpoch — the first moment the fingerprint can be checked
    // against the fully-armed engine (setSsrStore / setConsumeNetworkIono run
    // after initialize()) and the age against a real epoch time.  A refused
    // stage discards with a logged reason and the cold start proceeds
    // untouched.  writeStateSnapshot_ runs at the processEpoch tail on the
    // configured data-time cadence.
    uint64_t rtSnapshotFingerprint_() const;            ///< FNV-1a over the state-meaning config facts.
    void loadStateSnapshotStaged_();                    ///< Parse + stage the snapshot file (initialize tail).
    void applyStagedStateSnapshot_(RtEpochContext& ctx);///< One-shot: age/fingerprint gate, layout rebuild, wholesale x/P + baselines restore.
    void writeStateSnapshot_(RtEpochContext& ctx);      ///< Cadence-gated atomic snapshot write.

    // =====================================================================
    // initialize() seams (RealTimeEngineInit.cpp)
    // =====================================================================
    // Construction phase, run once before any frame is decoded.  ORDER IS A
    // DEPENDENCY CHAIN, not a style choice: the product parsers feed the
    // StateManager a-priori, which the processing components bind to, which the
    // resolver and the state seed then read.  seedInitialState() in particular
    // MUST follow initProcessingComponents() — PppModel::initializeState would
    // otherwise clobber the a-priori with ppp_state_'s zero position, which on a
    // live stream (no RINEX APPROX POSITION) leaves every satellite below the
    // mask and the filter unable to bootstrap.

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
    // The observation-buffering loop and the broadcast-ephemeris push are
    // deliberately NOT cut: both call anonymous-namespace helpers
    // (isSystemActive, refreshGlonassSlots) that are file-local to
    // RealTimeEngine.cpp by design, and hoisting those into a new internal
    // header to relocate 51 lines is a worse trade than leaving them in place.

    /// Push decoded SSR records under the per-stream RTA-8(b) accept policy.
    /// @return the number of records the policy DROPPED (never pushed).
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
    /// L10: consumer-side SSR-application trace (network.trace_ssr_consumer).  Opened in
    /// initialize() into the same "logs" dir as the producer traces; one row per modeled
    /// sat per processed epoch recording correction age / clk-applied / URA R-inflation /
    /// disc counters.  Never opened when the toggle is off (byte-identical).
    std::ofstream                        ssr_consumer_trace_;

    /// Assembles broadcast ephemeris from raw SBF nav subframes (GPSRawCA/
    /// GALRawINAV/BDSRaw) — persistent across blocks, feeds LiveEphemerisStore.
    SubframeAssembler                    subframe_assembler_;

    PppState ppp_state_{};
    Eigen::Vector3d initial_position_{ 0.0, 0.0, 0.0 };

    /// Non-owning SSR store override; nullptr -> process-wide singleton.
    /// Set via setSsrStore() for the closed-loop consumer (see header).
    const SsrCorrectionStore* ssr_store_override_{ nullptr };

    /// Card 7: the CLOCK-routed store (per setSsrStore's consume_clock), the
    /// store GnssCorrections tallies orbit-echo mismatches on.  The engine's
    /// ssr_store_override_ above is BIAS-routed (consume_bias), so under a
    /// clock-on/bias-off profile the two differ — the echo summary must read
    /// this one or a split consumer's mismatches stay invisible forever.
    const SsrCorrectionStore* clock_store_override_{ nullptr };

    /// Consume the network slant-iono supply as a tight per-sat prior (see
    /// @ref setConsumeNetworkIono).  Default false ⇒ processEpoch builds no iono
    /// pseudo-observation (byte-identical).
    bool consume_network_iono_{ false };

    /// Most recent epoch's AR result + whether AR ran (closed-loop metrics).
    AmbiguityResolution last_ar_result_{};
    bool last_ar_ran_{ false };

    /// When true, AR ignores the MW WL feed and runs pure MLAMBDA on the KF float
    /// (closed-loop network-phase-bias validation; see setArIgnoreMwWl).
    bool ar_ignore_mw_wl_{ false };

    uint64_t  epoch_count_{ 0 };
    bool      first_epoch_{ true };

    // processing.max_duration_s data-time cutoff (0 = unlimited).  The window
    // is anchored on the first FULLY-processed epoch (deferred warmup epochs
    // never reach the anchor site), so the budget covers real estimation time.
    bool      max_duration_anchored_{ false }; ///< True once the cutoff window has been anchored.
    double    max_duration_anchor_s_{ 0.0 };   ///< Continuous GPS seconds of the first processed epoch.
    bool      max_duration_reached_{ false };  ///< Latched once the elapsed data-time span hits the limit.

    // Corrections-ready startup gate (estimation.defer_until_corrections_ready).
    // While armed and unsatisfied, processEpoch defers ALL estimation (no
    // predict/update) until every modeled above-mask sat is broadcast+SSR.
    bool      estimation_started_{ false }; ///< Gate satisfied — estimation has begun (latched).
    uint64_t  warmup_epochs_{ 0 };          ///< Count of deferred epochs (throttled progress logging).
    GnssTime  warmup_start_time_{ 0, 0.0 }; ///< First deferred epoch time (optional timeout fallback).

    bool      apriori_seeded_from_pvt_{ false };  ///< Logged once when the a-priori is taken from PVTGeodetic.
    bool      apriori_seeded_from_station_{ false }; ///< Logged once when the a-priori is taken from an RTCM 1005/1006 station ARP.
    bool      arp_blunder_warned_{ false };          ///< RTA-12(3): warned once that a decoded 1005/1006 ARP failed the realtime.arp_blunder_gate_m cross-check against the existing a-priori.
    bool      warned_ssr_cb_no_ifb_{ false };        ///< A5: warned once that network code biases are consumed while estimate_ifb is off (no rx code-bias state to absorb a datum re-reference).
    // A5 (P1-4): per-(sat|signal) record of whether the SSR phase bias was APPLIED (fresh
    // within the staleness gate) at the previous epoch.  Only maintained when the bias
    // staleness gate (enable_ssr_bias_max_age) is on; a fresh↔stale flip is a full-bias
    // phase step with no producer disc-counter change, so the ambiguity is reset on the flip.
    std::map<std::string, bool> ssr_phase_bias_applied_;
    // A5 code analog: per-("CB|sat|signal") record of whether the SSR code bias was
    // APPLIED (present + fresh within the staleness gate) at the previous epoch.  Only
    // maintained when the gate is on; a fresh↔absent flip — including a signal silently
    // VANISHING from a wholesale-replacing producer push, which carries no disc counter —
    // steps the applied code correction by the full bias, so the affected constellation's
    // receiver code-bias states are re-inflated on the flip.
    std::map<std::string, bool> ssr_code_bias_applied_;
    /// RTA-3: last observed network solution-generation id from the SSR store
    /// (0 = never seen).  A CHANGE of a present id means the producer RESTARTED
    /// and re-realized its whole datum (every phase-bias lump / code-bias level
    /// at once, with no per-signal disc-counter continuity — a restarted
    /// producer's counters re-seed at 0, so a warm consumer holding 0 sees 0→0 =
    /// "no change"); the engine reacts with a global datum break: reset ALL
    /// ambiguities + re-inflate every receiver code-bias state.  An absent id
    /// (RTCM streams / pre-RTA-3 products) never reacts — byte-identical.
    uint32_t  last_ssr_generation_id_{ 0 };
    // --- RT filter-state snapshot (RealTimeEngineStateSnap.cpp) -----------
    /// Parsed-but-unapplied snapshot staged by loadStateSnapshotStaged_; applied
    /// or discarded (one-shot) at the top of the first processEpoch.  Never set
    /// unless realtime.state_snapshot_path is non-empty.
    std::unique_ptr<struct RtStateSnapshot> staged_rt_snap_;
    double    rt_snap_last_write_s_{ -1.0 };   ///< Continuous GPS seconds of the last snapshot write (-1 = never).
    bool      rt_snap_layout_warned_{ false }; ///< One-shot warn for an unserializable state layout (unknown dynamic index).
    /// RTA-10: last observed network-producer epoch-status STATE from the SSR
    /// store's heartbeat surface (nullopt = never seen).  Edge-trigger for the
    /// one-shot transition log (ok→withheld_sigma0 etc.): the first sighting
    /// logs once, then only CHANGES log.  Absent heartbeats (live RTCM streams,
    /// pre-RTA-10 products, network.emit_epoch_status_record off) never react —
    /// byte-identical console output for legacy content.  Deeper integrity
    /// wiring (gating measurement usage on producer status) is RTF-9 scope.
    std::optional<NetworkProducerStatus::State> last_producer_status_;
    double    prev_tow_{ 0.0 };
    GnssTime  prev_epoch_time_{ 0, 0.0 };
    uint64_t  backlog_warn_ctr_{ 0 };          ///< Throttle counter for the obs-backlog warning (every Nth lagging epoch).

    /// RTB-1(a): PERSISTENT RTCM parser — one per engine, reused across every
    /// frame from every stream (primary / corrections / ephemeris share the
    /// GPS timeline).  Persistence is what gives the parser's stream-time
    /// tracker its continuity: the old parser-per-frame pattern re-cold-started
    /// the week promotion off the host clock on EVERY frame.  clear() runs at
    /// the top of feedRtcmFrame (outputs are per-frame; the tracker survives).
    RtcmParser                      rtcm_parser_;

    /// RTB-3: MSM lock-time history for the LLI 0x1 derivation at epoch
    /// assembly (armed by realtime.msm_lli_from_locktime; station key "" —
    /// the multi-station front-end will key per stream).
    LockTimeTracker                 lock_time_tracker_;

    // Observation buffer for epoch assembly from streaming frames.
    GnssTime                        pending_epoch_time_{ 0, 0.0 };
    std::vector<RinexObservation>   pending_obs_;

    // RTB-1(b): receipt stamps for receive→process latency accounting.
    // pending_* tracks the FIRST frame that contributed observations to the
    // epoch currently assembling; processing_* is snapshotted for the epoch
    // handed to processEpoch (valid only on the RTCM assembly path — direct
    // feeds and held-epoch re-drains have no stamp).  Diagnostics only.
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
    bool      processing_synced_epoch_{ false };///< True while draining a held epoch — bypasses skip-to-latest (the held epoch is the intended SSR-aligned target).
    bool      draining_final_{ false };         ///< True during flush() — bypasses both obs↔SSR hold and skip-to-latest so the final epoch is never lost.
    uint64_t  sync_hold_warn_ctr_{ 0 };         ///< Throttle for the obs↔SSR hold debug line.
};

} // namespace GnssProcessor
