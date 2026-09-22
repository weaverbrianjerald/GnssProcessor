#pragma once
/**
 * @file SsrCorrectionStore.hpp
 * @brief Singleton holding the most-recent SSR corrections per satellite.
 *
 * Sits beside @c ProductCache.  ProductCache holds static / batch products
 * (SP3 orbits, RINEX-CLK clocks, ATX antennas, BIA biases, ERP polar
 * motion).  SsrCorrectionStore holds streaming SSR overlays that the
 * realtime engine pushes from decoded RTCM 1057-1068 / 1240-1270 / 4076
 * frames.  Consumers (@c Geometry::getSatellitePV,
 * @c GnssCorrections::getSatelliteClockCorrection,
 * @c MeasurementModel) query the store and apply corrections on top of
 * the broadcast / SP3 baseline.  When the store is empty, all queries
 * return "no correction" and the caller falls through to broadcast-only.
 *
 * Threading: a single mutex protects the per-satellite map.  Real-time
 * pushes happen on the NTRIP receive thread; engine queries happen on
 * the per-epoch processing thread.  The mutex window covers a copy of
 * the @c SatRecord, so query callers never hold the lock during the
 * subsequent RAC-to-ECEF transform.
 */

#include "rtcm/RtcmTypes.hpp"
#include "products/SignalDeclaration.hpp"   // Card SID phase 5: the declared signal roster
#include <Eigen/Dense>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

/**
 * @brief RTA-10: the network producer's per-epoch STATUS heartbeat.
 *
 * Parsed from the product file's positional @c H record
 * (@c H,week,tow,status,sigma0,n_sats_published,withheld — written every epoch
 * when @c network.emit_epoch_status_record is armed, INCLUDING fully withheld
 * epochs, which carry no @c E line) and pushed into the consumer's
 * @c SsrCorrectionStore by the real-time product-tail wiring so the engine can
 * distinguish "the producer withheld this epoch" from "the producer died".
 * Deeper integrity wiring (gating measurement usage on producer status) is out
 * of scope here — RTF-9.
 */
struct NetworkProducerStatus {
    /// Producer disposition of one epoch.  @c Unknown = an unrecognized status
    /// token from a NEWER producer (forward-compatible: surfaced, never fatal).
    enum class State { Ok, Partial, WithheldSigma0, WithheldGate, WithheldShGate, Unknown };

    GnssTime    time{0, 0.0};          ///< Content epoch the heartbeat describes.
    State       state{State::Unknown}; ///< Epoch disposition.
    double      sigma0{0.0};           ///< Global within-epoch σ̂0 (0 = unavailable).
    int         n_sats_published{0};   ///< Satellites in the emitted epoch (0 on a withhold).
    /// Semicolon-joined withheld constellation letters (e.g. "C;R"); "-" when
    /// none; "*" on a whole-epoch withhold.
    std::string withheld{"-"};

    /// Map a wire status token ("ok", "partial", "withheld_sigma0",
    /// "withheld_gate", "withheld_sh") to its @ref State; anything else →
    /// @c Unknown (a NEWER producer's token — forward-compatible, never fatal).
    /// Operator-facing semantics per state: `docs/rt_service_operations.md` §2.1.
    static State parseState(const std::string& token);
    /// Inverse of @ref parseState (@c Unknown → "unknown").
    static const char* stateName(State s);
};

/**
 * @class SsrCorrectionStore
 * @brief Per-satellite "latest SSR record" cache, thread-safe.
 *
 * Records are keyed by satellite ID (e.g. "G01", "E11").  Each push
 * overwrites the previous value of that record type for that satellite.
 * Lookups are by satellite + maximum acceptable age in seconds; stale
 * records are reported as missing.
 */
class SsrCorrectionStore {
public:
    /// Returns the process-wide singleton instance (used by the live real-time
    /// pipeline).  Construction is the first call.
    static SsrCorrectionStore& getInstance();

    /// Default-constructible so callers can own a LOCAL instance instead of the
    /// process-wide singleton.  The network product emitter's closed-loop test
    /// (and future producer/consumer store injection) uses an owned instance to
    /// stay isolated from global state — a producer must never share the same
    /// store it consumes from, and unit tests must not pollute the singleton.
    SsrCorrectionStore() = default;
    ~SsrCorrectionStore() = default;

    SsrCorrectionStore(const SsrCorrectionStore&)            = delete;
    SsrCorrectionStore& operator=(const SsrCorrectionStore&) = delete;
    SsrCorrectionStore(SsrCorrectionStore&&)                 = delete;
    SsrCorrectionStore& operator=(SsrCorrectionStore&&)      = delete;

    // -----------------------------------------------------------------
    // Push API.  Each method overwrites that satellite's previous record
    // of the same type.  @p header carries the SSR epoch and IOD-SSR
    // shared by every per-satellite record in the message.
    // -----------------------------------------------------------------
    void pushOrbit(const SsrOrbitCorrection& rec, const SsrHeader& header);
    void pushClock(const SsrClockCorrection& rec, const SsrHeader& header);
    void pushCombined(const SsrCombinedCorrection& rec, const SsrHeader& header);
    void pushCodeBias(const SsrCodeBias& rec, const SsrHeader& header);
    void pushPhaseBias(const SsrPhaseBias& rec, const SsrHeader& header);
    void pushUra(const SsrUra& rec, const SsrHeader& header);
    /// Push a per-satellite slant-iono supply record.  Unlike the other push*
    /// methods this takes NO @c SsrHeader: the channel is an in-process consumer
    /// supply that carries its own @c received_time for the freshness gate, and
    /// it must NOT overwrite the shared @c SatRecord header that @c emitToStore
    /// already stamped with the real IOD-SSR / provider for the same satellite.
    void pushSlantIono(const SsrSlantIono& rec);
    /**
     * @brief FC-1: refresh a satellite's clock VALUE and AGE only, from a fast
     *        split-cadence tick.  Takes no @c SsrHeader, for the same reason
     *        @ref pushSlantIono does not.
     *
     * A fast tick re-measures the satellite clock between full solves; everything
     * else in the correction set belongs to the slow era and must be left exactly
     * as the last full epoch stamped it.  Concretely, this method deliberately does
     * NOT touch:
     *  - @c SatRecord::header — the shared IOD-SSR / provider / SSR epoch stamped by
     *    @c emitToStore.  Overwriting it would age the code-bias, phase-bias, URA and
     *    slant-iono channels off one clock refresh.
     *  - @c disc_counter — a fast row is a continuation of the same clock, never a
     *    discontinuity.  Advancing it would make every consumer reset its filter at
     *    the fast cadence.
     *  - @c c1, @c iod_ssr, @c iode, the orbit linkage, or @c combined_code_sigma_m.
     *
     * It also refuses to CREATE a record: a fast row may only refresh a clock a full
     * epoch already established, so a consumer can never be handed a satellite that
     * exists solely on the fast channel and has no bias set to go with it.
     *
     * @return true if an existing clock record was refreshed; false if the satellite
     *         had no clock record yet (the row is then correctly ignored).
     */
    bool pushFastClock(const SsrClockCorrection& rec);
    void pushHighRateClock(const SsrHighRateClock& rec, const SsrHeader& header);

    /**
     * @brief Aggregated orbit + clock correction for one satellite.
     *
     * Built on demand by merging the satellite's most recent orbit
     * (or combined) record with its most recent clock record.  When a
     * combined record is present and not stale, it wins over separate
     * orbit + clock messages.
     */
    struct OrbitClockCorrection {
        Eigen::Vector3d delta_rac{Eigen::Vector3d::Zero()};      ///< Radial / along-track / cross-track delta (m).
        Eigen::Vector3d delta_rac_dot{Eigen::Vector3d::Zero()};  ///< RAC velocity delta (m/s).
        double          c0{0.0};                                  ///< Clock C0 (m).
        double          c1{0.0};                                  ///< Clock C1 (m/s).
        double          c2{0.0};                                  ///< Clock C2 (m/s^2).
        double          hr_clock{0.0};                            ///< High-rate clock delta (m); 0 if not present.
        GnssTime        epoch_time{0, 0.0};                       ///< SSR content reference epoch of the CLOCK correction — dt base for applyClockToBias.
        GnssTime        orbit_epoch_time{0, 0.0};                 ///< SSR content reference epoch of the ORBIT correction — dt base for applyOrbitToPosition (equals epoch_time for a combined record).
        int             iode{-1};                                 ///< Broadcast IODE the ORBIT applies to; -1 == unknown → keeps iodeMatches() permissive. A standalone clock subtype carries no orbit IODE and leaves this at -1 (see getOrbitClock; RT0-1).
        /// RTF-2 (Card 3): broadcast IODE the CLOCK C0 is referenced to (IGS-SSR
        /// matching convention).  Distinct from @c iode (the ORBIT anchor): a
        /// standard SSR clock carries no independent IODE, so this INHERITS the
        /// paired orbit's @c iode (byte-identical to the pre-card clock gate,
        /// which gated the clock on @c iode too); the @c brdc_rts network producer
        /// stamps its own value (SsrClockCorrection.iode ≥ 0) which then wins.
        /// -1 = unknown (permissive).  GnssCorrections gates CLOCK application on
        /// @c iodeMatches(brdc_iode, clock_iode); ORBIT application stays on @c iode.
        int             clock_iode{-1};
        /// RTF-2 (Card 3): orbit coherence echo carried from the winning clock /
        /// combined record (IOD-SSR + content epoch of the RTS orbit the clock was
        /// estimated against).  Default -1 / {0,0} = not stamped.  Consumed by the
        /// Card-7 consumer echo check (@ref checkOrbitEcho).
        int             orbit_iod_ssr{-1};
        GnssTime        orbit_epoch{0, 0.0};
        /// Card 7 (RTF-2): the OWN IOD-SSR of the ORBIT record merged into this
        /// correction (@c SsrOrbitCorrection.iod_ssr / @c SsrCombinedCorrection.iod_ssr)
        /// — the APPLIED orbit's identity, as opposed to @c orbit_iod_ssr (the
        /// producer's ECHO of the orbit it estimated against, carried on the clock
        /// record).  The consumer echo check compares the two.  -1 = no orbit part
        /// merged.  For a combined record the echo and this field are the same
        /// value by construction (orbit/clock coherence is guaranteed in-message).
        int             applied_orbit_iod_ssr{-1};
        bool            has_orbit{false};                         ///< True if orbit fields are valid.
        bool            has_clock{false};                         ///< True if clock fields are valid.
        /// M4 (contract v2): formal 1σ (m) of the COMBINED CODE correction for this
        /// satellite (clock + code bias at a reference signal, cross-cov included),
        /// carried from the winning clock/combined record.  0 for decoded streams and
        /// when the producer's @c network.emit_combined_sigma is off.  The consumer
        /// folds it (age-inflated) into a CODE row's variance under
        /// @c closed_loop.consume_combined_sigma.  Meaningful only when @c has_clock.
        double          combined_code_sigma_m{0.0};
        /// M4 (contract v2): correlation of this satellite's combined code & combined
        /// phase corrections (0 = absent).  Carried for a future correlated-measurement
        /// consumer; the present diagonal-R consumer does not apply it.
        double          code_phase_corr{0.0};
        /// RTB-5(b): satellite reference point of the ORBIT part (@c APC for
        /// legacy-SSR decodes, @c CoM for IGS-SSR 4076 / network products) —
        /// copied from the winning orbit/combined record.  Meaningful only
        /// when @c has_orbit; stays the @c CoM default otherwise.  Read by
        /// @c Geometry at the orbit-overlay apply sites so @c PppModel can
        /// skip its satellite-PCO term under
        /// @c realtime.honor_ssr_reference_point (double-count guard).
        SsrReferencePoint orbit_reference_point{SsrReferencePoint::CoM};
    };

    /**
     * @brief Look up the merged orbit+clock correction for a satellite.
     * @param sat        Satellite ID (e.g. "G01").
     * @param query_time Reference time of the query, used for the staleness check.
     * @param max_age_s  Maximum acceptable age (query_time - record_time) in seconds.
     *                   Defaults to 60 s — typical IGS-RTS update interval.
     * @return Populated correction, or @c std::nullopt if there is no
     *         record for this satellite or all records are stale.
     */
    std::optional<OrbitClockCorrection> getOrbitClock(
        const std::string& sat,
        const GnssTime&    query_time,
        double             max_age_s = 60.0) const;

    /**
     * @brief Look up the code bias for a (sat, signal) pair.
     * @return Bias in meters, or 0.0 if absent.
     */
    double getCodeBias(const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Test whether the stream supplies a code bias for a (sat, signal).
     *
     * Presence check mirroring @c getCodeBias's lookup: true iff the satellite
     * has a code-bias record whose @c bias_per_signal map contains the EXACT
     * RINEX obs code.  A present-but-zero entry counts as covered — the
     * provider's bias set defines that signal's datum frame regardless of the
     * numeric value, so an estimator must not also estimate it.  Distinct from
     * @c getCodeBias != 0.0, which cannot tell a genuine-zero bias from an
     * absent signal.
     *
     * Used by @c MeasurementModel to suppress redundant per-(sat, signal)
     * sat-bias STATE allocation for signals the live stream already corrects;
     * it must agree exactly with @c getCodeBias so a covered signal is never
     * both estimated AND SSR-corrected on the same row.
     *
     * @param sat            Satellite ID (e.g. "G01").
     * @param rinex_obs_code Exact RINEX obs code (e.g. "C1C", "C5Q").
     * @return true if the stream carries a code bias for this exact pair.
     */
    bool hasCodeBias(const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Look up the phase bias entry for a (sat, signal) pair.
     */
    std::optional<SsrPhaseBiasEntry> getPhaseBias(
        const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Look up the network code-bias discontinuity counter for a (sat,
     *        signal) pair.
     *
     * Returns @c std::nullopt when the stream carries no code bias for the pair
     * OR carries one without a discontinuity counter (every RTCM/IGS-SSR-decoded
     * code bias — the wire format has no such field).  A value is present only
     * for biases emitted by the network producer (@c NetworkProductEmitter) with
     * @c network.estimate_sat_code_bias on.  The consumer uses an increment of
     * this counter to re-inflate the affected receiver code-bias state when the
     * producer's code-bias datum re-references, the analog of the phase-bias
     * discontinuity → ambiguity reset.
     *
     * @param sat            Satellite ID (e.g. "G01").
     * @param rinex_obs_code Exact RINEX obs code (e.g. "C1C", "C5Q").
     * @return The counter, or @c std::nullopt if untracked.
     */
    std::optional<int> getCodeBiasDiscontinuity(
        const std::string& sat, const std::string& rinex_obs_code) const;

    /**
     * @brief Look up the network clock-discontinuity counter for a satellite.
     *
     * Returns @c std::nullopt when the satellite has no clock record; otherwise the
     * record's @c disc_counter, which is 0 for every RTCM/IGS-SSR-decoded clock (the
     * wire format has no such field) and for network products emitted with
     * @c network.emit_clock_disc off.  A non-zero, incrementing value is produced
     * only by @c NetworkProductEmitter (A2) when the producer's per-constellation
     * clock datum re-references (a clock-pivot handover).  The consumer uses an
     * increment to note the re-reference (the clock analog of the phase-bias
     * discontinuity → ambiguity reset).
     *
     * @param sat Satellite ID (e.g. "G01").
     * @return The counter, or @c std::nullopt if the satellite has no clock record.
     */
    std::optional<int> getClockDiscontinuity(const std::string& sat) const;

    /**
     * @brief Look up the per-satellite URA (User Range Accuracy) 1σ in meters (B1).
     *
     * Returns the pushed @c SsrUra::ura_m when a URA record is present and fresh
     * (within @p max_age_s of @p query_time), else @c std::nullopt.  Populated by
     * the network producer with @c network.emit_ura on (EWMA(σ̂0)×formal clock σ);
     * the consumer folds it into the measurement variance on rows using that
     * satellite's corrections.  Mirrors @c getOrbitClock's staleness gate.
     */
    std::optional<double> getUra(const std::string& sat, const GnssTime& query_time,
                                 double max_age_s = 60.0) const;

    /**
     * @brief Look up the per-satellite UNDIFFERENCED slant-iono supply record.
     *
     * Returns the pushed @c SsrSlantIono (slant delay on GPS L1 + per-LOS 1σ)
     * when a record is present and fresh (within @p max_age_s of @p query_time),
     * else @c std::nullopt.  Populated by the network producer for a closed-loop
     * consumer (@c closed_loop.consume_network_iono on) by evaluating the
     * co-estimated SH-VTEC field at the consumer's pierce points; the consumer
     * folds it into a tight ionosphere-weighted (BLUP) pseudo-observation on its
     * per-satellite slant-iono state.  Mirrors @c getUra's staleness gate.  The
     * whole struct is returned (not a bare value) because the consumer needs both
     * @c iono_m and @c sigma_m.
     */
    std::optional<SsrSlantIono> getSlantIono(const std::string& sat,
                                             const GnssTime& query_time,
                                             double max_age_s = 60.0) const;

    // --- A5: age-gated overloads (reject a code/phase-bias record older than
    // @p max_age_s relative to @p query_time, mirroring getOrbitClock's staleness
    // gate).  The no-time overloads above are retained and never gate, so existing
    // callers are byte-identical.  A run of gate-withheld network epochs would
    // otherwise feed unboundedly stale biases silently. ------------------------
    double getCodeBias(const std::string& sat, const std::string& rinex_obs_code,
                       const GnssTime& query_time, double max_age_s) const;
    std::optional<SsrPhaseBiasEntry> getPhaseBias(
        const std::string& sat, const std::string& rinex_obs_code,
        const GnssTime& query_time, double max_age_s) const;
    std::optional<int> getCodeBiasDiscontinuity(
        const std::string& sat, const std::string& rinex_obs_code,
        const GnssTime& query_time, double max_age_s) const;
    /// Age-gated presence check: like the no-time @c hasCodeBias (present-but-zero
    /// counts as covered) but false once the record is stale — so coverage agrees
    /// with what the aged @c getCodeBias actually applies.
    bool hasCodeBias(const std::string& sat, const std::string& rinex_obs_code,
                     const GnssTime& query_time, double max_age_s) const;

    /// SSR reference epoch of the most recent record pushed for @p sat
    /// (from the message header), or @c std::nullopt if none.
    std::optional<GnssTime> getSsrEpoch(const std::string& sat) const;

    /// L10: received-time of the most recent phase-bias record pushed for @p sat
    /// (the record-level stamp @c getPhaseBias's freshness gate uses), or
    /// @c std::nullopt if none.  @c SsrPhaseBiasEntry itself carries no timestamp;
    /// this exposes the enclosing record's @c received_time so a consumer trace can
    /// age the phase bias exactly as the apply path does.
    std::optional<GnssTime> getPhaseBiasTime(const std::string& sat) const;

    /// @brief Freshest SSR content epoch across ALL satellites in the store.
    ///
    /// Returns the maximum message header epoch_time held for any satellite, or
    /// @c std::nullopt when the store is empty / no header has been set.  Unlike
    /// @c getSsrEpoch (per-satellite) this is the stream-wide "newest correction
    /// content received" signal.  The real-time obs↔SSR sync uses it to bound how
    /// far ahead of the corrections the obs stream may run: an obs epoch newer
    /// than (newestSsrEpoch + ssr_max_age) has no usable SSR for any satellite and
    /// is held until the stream advances (see RealTimeEngine obs↔SSR sync).
    std::optional<GnssTime> newestSsrEpoch() const;

    /// IOD-SSR of the most recent record pushed for @p sat, or -1 if none.
    int getSsrIod(const std::string& sat) const;

    /**
     * @brief RTA-3: record the network producer's solution-GENERATION id.
     *
     * A store-wide (not per-satellite) epoch-header field, set by
     * @c NetworkProductEmitter::emitToStore when the producer stamps a generation
     * id (@c network.emit_generation_id).  Unlike the per-record IOD-SSR (a
     * per-epoch increment) this is a per-RUN marker: it distinguishes one datum
     * realization from another across producer restarts.  0 is reserved for
     * "unset" and is ignored by @c getGenerationId.
     */
    void setGenerationId(uint32_t id);

    /**
     * @brief RTA-3: the network solution-generation id, if one was ever set.
     *
     * Returns @c std::nullopt when no producer stamped a generation id (live RTCM
     * streams and pre-RTA-3 network products — the wire format has no such field),
     * so a consumer gating on presence never reacts to legacy content.  A CHANGE
     * of a present value between epochs means the producer RESTARTED and
     * re-realized its whole datum (every phase-bias lump and code-bias level at
     * once, with no per-signal counter continuity): the consumer must treat it as
     * a global datum break — reset all network-corrected ambiguities and
     * re-inflate its receiver code-bias states.
     */
    std::optional<uint32_t> getGenerationId() const;

    /**
     * @brief RTA-10: record the network producer's latest epoch-status heartbeat.
     *
     * Store-wide (not per-satellite), pushed by the real-time product-tail wiring
     * (@c ApplicationRealtime) for every @c H record the tail consumes — including
     * heartbeats of fully WITHHELD epochs, which complete no product record.  The
     * engine reads it via @ref getProducerStatus to log status transitions.
     */
    void setProducerStatus(const NetworkProducerStatus& status);

    /**
     * @brief RTA-10: the latest producer epoch-status heartbeat, if any was pushed.
     *
     * @c std::nullopt when no producer heartbeat has been seen (live RTCM streams,
     * pre-RTA-10 products, or @c network.emit_epoch_status_record off), so a
     * consumer gating on presence never reacts to legacy content.
     */
    std::optional<NetworkProducerStatus> getProducerStatus() const;

    /**
     * @brief Card SID phase 5 (E2): record the producer's DECLARED SIGNAL ROSTER.
     *
     * Store-wide (not per-satellite), pushed by the in-process producer at each
     * emit and by the file reader's @c replay for every @c N declaration block
     * it parses.  A declaration REPLACES the previous one wholesale, which is
     * the producer's own semantics: the block names the whole roster.
     *
     * Configuration-like rather than data-like, so it survives @ref clear for
     * the same reason @ref setOrbitEchoMaxSkew does: it describes what the
     * producer carries, not what it sent at one epoch.
     */
    void setDeclaredRoster(const NetworkSignalDeclaration& decl);

    /**
     * @brief Card SID phase 5 (E2): the declared signal roster, if one was seen.
     *
     * @c present false (the default) on every pre-card wire, every live RTCM
     * stream and every producer that did not arm @c network.ledger_emit, so a
     * consumer gating on presence never infers anything from a legacy stream.
     * Returned BY VALUE: the store is shared and mutex-guarded, and the caller
     * reads the roster per epoch rather than per row.
     */
    NetworkSignalDeclaration declaredRoster() const;

    /**
     * @brief M4 (contract v2): set the combined-correction age-scaling coefficient.
     *
     * Store-wide (not per-satellite), the producer's @c network.combined_sigma_agek
     * advertised on the product S-line.  A consumer inflates a network-corrected
     * row's combined variance as σc²·(1 + agek·Δt_age²) (correction content age
     * Δt_age in s).  Set by the file-tail wiring when it parses an S contract
     * carrying an @c agek= token (or by an in-process producer driver from config).
     * 0 (the default) ⇒ no age inflation.  Survives @ref clear (configuration, not
     * data), like @ref setOrbitEchoMaxSkew.
     */
    void setCombinedSigmaAgeCoeff(double agek);

    /// M4 (contract v2): the combined-correction age-scaling coefficient (0 = none).
    double combinedSigmaAgeCoeff() const;

    /// True iff no records have been pushed.
    bool isEmpty() const;

    /// Number of satellites with at least one SSR record.
    std::size_t size() const;

    /**
     * @brief Compact per-constellation census of cached records, for diagnostics.
     *
     * Returns a string like @c "G[o8 c8 cb8 pb8] E[o4 c4 cb4 pb4] C[o4 c4 cb0 pb4]"
     * where each constellation letter is followed by the count of satellites
     * carrying an orbit (or combined), a clock (or combined), a code bias, and a
     * phase bias record respectively.  Used by the realtime startup gate to
     * localise an SSR problem to decode/store (counts are zero) vs. application
     * (counts are non-zero but clocks still don't apply — an IODE-match issue).
     */
    std::string censusSummary() const;

    /**
     * @brief Per-epoch SSR coverage for a specific set of satellites.
     *
     * Unlike censusSummary() (which tallies the WHOLE store), this counts only
     * @p sats — the satellites actually being modeled this epoch — so the
     * realtime engine can log how many sats in the solution have a FRESH SSR
     * orbit / clock and a code / phase bias available.  "Available" means a
     * record is present within @p max_age_s; whether the orbit/clock additionally
     * *applies* depends on the IODE gate (see getSatelliteClockCorrection), so
     * the engine pairs this with the true clock-applied (cm-grade) count.
     */
    struct Coverage {
        int total      = 0;   ///< Satellites queried.
        int orbit      = 0;   ///< With a fresh SSR orbit (or combined) record.
        int clock      = 0;   ///< With a fresh SSR clock (or combined / high-rate) record.
        int code_bias  = 0;   ///< With a code-bias record.
        int phase_bias = 0;   ///< With a phase-bias record.
    };
    Coverage coverageFor(const std::vector<std::string>& sats,
                         const GnssTime& time, double max_age_s = 60.0) const;

    /// Drop every record.
    void clear();

    /**
     * @brief Apply an orbit RAC delta to a broadcast ECEF position.
     *
     * Frame convention (RTCM 10403.3 sec. 3.5.13):
     *   e_along  = vel / |vel|
     *   e_cross  = (pos × vel) / |pos × vel|
     *   e_radial = e_along × e_cross
     *
     * The propagated delta (delta_rac + delta_rac_dot · dt) is rotated
     * into ECEF and SUBTRACTED from @p brdc_pos to yield the precise
     * position (RTCM defines delta = broadcast − precise).
     */
    static Eigen::Vector3d applyOrbitToPosition(
        const Eigen::Vector3d& brdc_pos,
        const Eigen::Vector3d& brdc_vel,
        const Eigen::Vector3d& delta_rac,
        const Eigen::Vector3d& delta_rac_dot,
        double                 dt);

    /**
     * @brief Combine clock polynomial and high-rate delta into seconds.
     * @return Clock correction in seconds; ADD to broadcast clock bias.
     */
    static double applyClockToBias(double c0, double c1, double c2, double hr_clock, double dt);

    // -----------------------------------------------------------------
    // RTA-9(b): clock staleness ladder.
    // -----------------------------------------------------------------

    /**
     * @brief Rung of the SSR clock staleness ladder (RTA-9(b)).
     *
     * The clock apply path (@c GnssCorrections::getSatelliteClockCorrection)
     * historically had a two-state gate: fresh (age ≤ @c processing.ssr_max_age_s
     * → applied) or rejected (→ broadcast-grade, and wiped entirely under
     * @c drop_broadcast_grade).  With @c processing.ssr_clock_stale_hold_factor
     * > 0 a middle rung opens: a clock whose age lies in
     * (max_age, hold_factor × max_age] is STILL APPLIED — the C0 + C1·dt
     * polynomial extrapolates over the true dt — while the consuming
     * measurement rows are de-weighted through the URA-staleness machinery in
     * @c MeasurementModel (the ZOH q²·dt³/3 growth; see the R-inflation site
     * there).  Beyond the hold window the original cliff applies.
     */
    enum class ClockAgeRung {
        Fresh,      ///< age ≤ max_age: applied at full weight (the historic accept band).
        StaleHeld,  ///< max_age < age ≤ hold_factor × max_age: applied + R-inflated (rung armed only when hold_factor > 0).
        Rejected    ///< beyond the hold window (or far-future desync): broadcast-grade cliff.
    };

    /**
     * @brief Effective clock APPLY window for the staleness ladder (RTA-9(b)).
     *
     * @c hold_factor ≤ 0 (the default 0 = rung off) returns @p max_age_s
     * unchanged, keeping the historic gate byte-identical.  A positive factor
     * returns max_age × max(factor, 1) — factors below 1 cannot SHRINK the
     * window (they would silently tighten the base gate), so they degenerate
     * to "no hold".
     *
     * @param max_age_s        Base clock max age (@c processing.ssr_max_age_s).
     * @param stale_hold_factor @c processing.ssr_clock_stale_hold_factor (0 = off).
     * @return The age limit to pass to @ref getOrbitClock for the CLOCK path.
     */
    static double clockHoldWindow(double max_age_s, double stale_hold_factor) {
        if (stale_hold_factor <= 0.0) return max_age_s;
        return max_age_s * (stale_hold_factor > 1.0 ? stale_hold_factor : 1.0);
    }

    /**
     * @brief Classify a clock-record age against the staleness ladder (RTA-9(b)).
     *
     * Pure decision function so the ladder is unit-testable independent of the
     * store / corrections plumbing.  Mirrors the file-local @c freshEnough
     * three-region semantics on the FUTURE side: a record more than one full
     * window in the future relative to the query is the obs↔SSR desync case
     * and is @c Rejected; a mild negative age (receipt jitter) is @c Fresh.
     *
     * @param age_s            query_time − record_time in seconds (may be negative).
     * @param max_age_s        Base clock max age (@c processing.ssr_max_age_s).
     * @param stale_hold_factor @c processing.ssr_clock_stale_hold_factor (0 = rung off).
     * @return @c Fresh / @c StaleHeld / @c Rejected.  @c StaleHeld is only
     *         reachable when the factor is > 1 (the window strictly widens).
     */
    static ClockAgeRung classifyClockAge(double age_s, double max_age_s,
                                         double stale_hold_factor) {
        const double window = clockHoldWindow(max_age_s, stale_hold_factor);
        if (age_s < -window)     return ClockAgeRung::Rejected;  // far-future desync
        if (age_s <= max_age_s)  return ClockAgeRung::Fresh;     // incl. mild negatives
        if (age_s <= window)     return ClockAgeRung::StaleHeld;
        return ClockAgeRung::Rejected;
    }

    // -----------------------------------------------------------------
    // RTA-13: obs-code attribute-mismatch visibility.
    // -----------------------------------------------------------------

    /**
     * @brief Enable the CODE-domain band-wise bias fallback (RTA-13, opt-in).
     *
     * Config @c processing.ssr_bias_band_fallback (default false).  When on,
     * @ref getCodeBias (and, symmetrically, @ref hasCodeBias — the two must
     * agree so a covered signal is never both estimated AND SSR-corrected)
     * accepts a same-band different-attribute sibling code bias when the exact
     * RINEX obs code is absent — e.g. a receiver tracking C5X consumes the
     * network's C5Q bias.  The residual attribute delta (a small intra-band
     * DCB) is absorbed by the receiver code-bias states downstream
     * (@c estimate_ifb, armed by the network-consumer profile).  NEVER applies
     * to PHASE biases (@ref getPhaseBias): a wrong-attribute phase bias
     * destroys the integer property, so phase stays exact-match + count-only.
     * The per-signal discontinuity counters (@ref getCodeBiasDiscontinuity)
     * intentionally do NOT follow the sibling — a datum re-reference signalled
     * only under the sibling code is not replayed onto the fallback consumer;
     * the receiver code-bias state absorbs that too (documented limitation).
     *
     * Set on the OWNED store by the injection site (@c Application::runRealTime)
     * and on the singleton by @c RealTimeEngine::initialize; default-off keeps
     * every lookup byte-identical.
     */
    void setCodeBiasBandFallback(bool enable);

    /// RTA-13: whether the CODE-domain band-wise bias fallback is armed.
    bool codeBiasBandFallback() const;

    /**
     * @brief RTA-13: per-(sat, signal, domain) attribute-mismatch counters.
     *
     * A counter increments each time a bias lookup (@ref getCodeBias /
     * @ref getPhaseBias, both overloads) MISSES on the exact RINEX obs code
     * while the same satellite carries a bias for a SAME-BAND sibling code
     * (identical type letter + band digit, different tracking attribute) —
     * the silent "receiver tracks C5X, network emitted only C5Q → bias=0"
     * failure.  Counted whether or not the fallback
     * (@ref setCodeBiasBandFallback) resolves it, so the mismatch stays
     * visible either way.  Keys are @c "sat|signal|domain" with domain
     * @c 'C' (code) or @c 'L' (phase), e.g. @c "G01|C5X|C".  Reset by
     * @ref clear.  @c RealTimeEngine logs a one-line summary periodically and
     * at end-of-run.
     *
     * @return Copy of the counter map (taken under the store mutex).
     */
    std::map<std::string, std::uint64_t> biasAttributeMismatchCounts() const;

    // -----------------------------------------------------------------
    // Card 7 (RTF-2): orbit-coherence echo verification.
    // -----------------------------------------------------------------

    /**
     * @brief Verdict of the Card-7 orbit-coherence echo check.
     *
     * A @c brdc_rts network clock is estimated against a specific RTS orbit
     * update; broadcast radial-orbit error absorbs 1:1 into that clock, so a
     * consumer applying a DIFFERENT orbit update introduces a silent dm–m
     * range bias invisible to residual QC (conventions §2).  The producer
     * therefore echoes the orbit identity (IOD-SSR + content epoch) on each
     * clock record; the consumer compares it against the orbit it actually
     * applies.
     */
    enum class OrbitEchoVerdict {
        NotStamped,      ///< Clock record carries no echo (standard SSR / non-brdc_rts) — nothing to verify.
        Match,           ///< Applied orbit identity agrees with the echo (within the skew budget when armed).
        NoAppliedOrbit,  ///< Echo stamped but the consumer applies NO SSR orbit for this satellite (bare broadcast orbit — the producer's orbit delta is missing entirely).
        IodSsrMismatch,  ///< Applied orbit's own IOD-SSR differs from the echoed one.
        EpochSkew        ///< IOD-SSR agrees but the applied orbit's content epoch is further from the echoed epoch than the armed budget (older/newer orbit update generation).
    };

    /**
     * @brief Pure classification of the orbit-coherence echo (Card 7).
     *
     * @param rec        Merged correction whose CLOCK is about to be applied
     *                   (carries the producer echo @c orbit_iod_ssr /
     *                   @c orbit_epoch).
     * @param applied    Merged correction the consumer's ORBIT overlay would
     *                   apply for the same satellite/epoch (from the orbit
     *                   store — @c GnssCorrections::orbitEchoStore); pass the
     *                   raw @ref getOrbitClock result.  @c nullopt or
     *                   @c !has_orbit ⇒ @c NoAppliedOrbit.
     * @param max_skew_s Orbit/clock age-skew budget (s).  The content-epoch
     *                   comparison runs ONLY when > 0 — with the budget unset
     *                   the check degrades to IOD-SSR identity + presence
     *                   (count-only diagnostics, RTA-13 style).
     * @return Verdict; @c NotStamped whenever @c rec carries no echo, so every
     *         pre-Card-7 stream short-circuits to "nothing to verify".
     */
    static OrbitEchoVerdict classifyOrbitEcho(
        const OrbitClockCorrection&                rec,
        const std::optional<OrbitClockCorrection>& applied,
        double                                     max_skew_s);

    /**
     * @brief Classify AND tally the orbit-coherence echo for one satellite.
     *
     * Calls @ref classifyOrbitEcho with this store's armed skew budget
     * (@ref setOrbitEchoMaxSkew) and increments the per-satellite mismatch
     * counter on any non-@c Match / non-@c NotStamped verdict — counted
     * whether or not the caller withholds, so the incoherence stays visible
     * even in count-only mode (the RTA-13 convention).  Keys are
     * @c "sat|reason" with reason ∈ { @c noorbit, @c iod, @c skew }.
     *
     * The WITHHOLD policy stays with the caller (@c GnssCorrections drops the
     * clock only when the budget is armed > 0); this method only observes.
     *
     * @param sat     Satellite ID (counter key prefix).
     * @param rec     Merged correction whose CLOCK is about to be applied.
     * @param applied The consumer's applied-orbit lookup for the same sat/epoch.
     * @return The verdict, so the caller decides application in one call.
     */
    OrbitEchoVerdict checkOrbitEcho(
        const std::string&                         sat,
        const OrbitClockCorrection&                rec,
        const std::optional<OrbitClockCorrection>& applied) const;

    /**
     * @brief Arm the Card-7 orbit/clock age-skew budget on this store (s).
     *
     * Config @c processing.network_consumer.orbit_clock_max_skew_s (default
     * 0 = the epoch-skew comparison is OFF and clock application is never
     * withheld; IOD-SSR identity mismatches are still counted when an echo is
     * present).  Set on the OWNED network-product store by the injection site
     * (@c Application::runRealTime) and on the singleton by
     * @c RealTimeEngine::initialize — the @c setCodeBiasBandFallback pattern.
     * Survives @ref clear (configuration, not data).
     */
    void setOrbitEchoMaxSkew(double max_skew_s);

    /// Card 7: the armed orbit/clock age-skew budget (s); 0 = withhold off.
    double orbitEchoMaxSkew() const;

    /**
     * @brief Card 7: per-satellite orbit-echo mismatch counters.
     *
     * Keys @c "sat|reason" (reason ∈ { @c noorbit, @c iod, @c skew }); see
     * @ref checkOrbitEcho.  Reset by @ref clear.  @c RealTimeEngine logs a
     * one-line summary periodically and at end-of-run, silent when empty.
     *
     * @return Copy of the counter map (taken under the store mutex).
     */
    std::map<std::string, std::uint64_t> orbitEchoMismatchCounts() const;

    /**
     * @brief Decide whether an SSR record is safe to apply to a given broadcast ephemeris.
     *
     * SSR corrections are deltas anchored to a specific broadcast
     * ephemeris record identified by its IODE.  When the rover's
     * broadcast nav has rolled to a newer IODE before the SSR provider
     * caught up (or vice versa), applying the SSR delta would land on
     * the wrong baseline orbit/clock and produce a degraded answer.
     *
     * The check is intentionally permissive when either side is
     * @c -1 ("unknown"):
     *   - SP3-derived @c OrbitData carries @c iode=-1, so post-processing
     *     paths (which never actually ought to apply SSR) silently
     *     pass the gate today.  Tightening this is one of the things
     *     a future broadcast-nav parser will enable.
     *   - Some SSR providers omit the IODE for certain message subtypes
     *     (rare).
     *
     * @param broadcast_iode  IODE of the closest broadcast-nav sample, or -1 if unknown.
     * @param ssr_iode        IODE the SSR record was issued against, or -1 if unknown.
     * @return true if the correction may be applied; false only when
     *         both IODEs are known (>= 0) and they differ.
     */
    static bool iodeMatches(int broadcast_iode, int ssr_iode) {
        if (broadcast_iode < 0 || ssr_iode < 0) return true;
        return broadcast_iode == ssr_iode;
    }

private:
    struct SatRecord {
        std::optional<SsrOrbitCorrection>    orbit;
        std::optional<SsrClockCorrection>    clock;
        std::optional<SsrCombinedCorrection> combined;
        std::optional<SsrUra>                ura;
        std::optional<SsrSlantIono>          slant_iono;
        std::optional<SsrHighRateClock>      hrclock;
        std::optional<SsrCodeBias>           code_bias;
        std::optional<SsrPhaseBias>          phase_bias;
        SsrHeader                            header{};   ///< Header of the most recent push (epoch/IOD-SSR).
    };

    mutable std::mutex                       mutex_;
    std::unordered_map<std::string, SatRecord> by_sat_;
    uint32_t                                 generation_id_{0};  ///< RTA-3: producer solution-generation id; 0 = unset.
    std::optional<NetworkProducerStatus>     producer_status_;   ///< RTA-10: latest producer epoch-status heartbeat; nullopt = never seen.
    /// Card SID phase 5: the producer's declared signal roster (see
    /// @ref setDeclaredRoster).  Configuration-like: survives clear().
    NetworkSignalDeclaration                 declared_roster_;

    /// RTA-13: CODE-domain band-wise bias fallback (see setCodeBiasBandFallback).
    /// Default false — every lookup byte-identical.
    bool code_bias_band_fallback_{false};

    /// RTA-13: per-(sat|signal|domain) attribute-mismatch counters (see
    /// biasAttributeMismatchCounts).  Written from const lookup methods under
    /// mutex_, hence mutable.  std::map keeps summary output deterministic.
    mutable std::map<std::string, std::uint64_t> bias_attr_mismatch_;

    /// Card 7: orbit/clock age-skew budget (s); 0 = epoch-skew check + withhold
    /// off (see setOrbitEchoMaxSkew).  Configuration — survives clear().
    double orbit_echo_max_skew_s_{0.0};

    /// M4 (contract v2): combined-correction age-scaling coefficient
    /// (see setCombinedSigmaAgeCoeff); 0 = no age inflation.  Configuration —
    /// survives clear().
    double combined_sigma_age_coeff_{0.0};

    /// Card 7: per-"sat|reason" orbit-echo mismatch counters (see
    /// checkOrbitEcho).  Written from the const check under mutex_, hence
    /// mutable.  std::map keeps summary output deterministic.
    mutable std::map<std::string, std::uint64_t> orbit_echo_mismatch_;

    /// RTA-13: find a same-band different-attribute sibling of @p code in a
    /// per-signal map (first two chars equal — type letter + band digit — key
    /// differs).  Returns m.end() when none.  Deterministic: std::map order.
    /// Caller holds mutex_.
    template <typename MapT>
    static typename MapT::const_iterator findBandSibling(const MapT& m,
                                                         const std::string& code) {
        if (code.size() < 2) return m.end();
        // RTA-13 fix: a combined iono-free key ("C1W_C5X") must NEVER band-fall
        // back to a raw single-signal sibling — its first two chars ("C1") would
        // spuriously match "C1C"/"C1W" and hand a wrong single-band code bias to
        // a combined observable (and inflate the mismatch counter even with the
        // fallback off).  Combined keys exact-miss and are skipped, restoring the
        // pre-RTA-13 return-0/nullopt behaviour for iono-free observables.
        if (code.find('_') != std::string::npos) return m.end();
        for (auto it = m.begin(); it != m.end(); ++it) {
            if (it->first != code && it->first.size() >= 2
                && it->first.find('_') == std::string::npos
                && it->first.compare(0, 2, code, 0, 2) == 0)
                return it;
        }
        return m.end();
    }

    /// RTA-13: record an exact-miss-with-sibling event (caller holds mutex_).
    void countBiasAttrMismatch(const std::string& sat, const std::string& code,
                               char domain) const {
        bias_attr_mismatch_[sat + "|" + code + "|" + domain] += 1;
    }
};

} // namespace GnssProcessor
