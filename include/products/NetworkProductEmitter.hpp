#pragma once
/**
 * @file NetworkProductEmitter.hpp
 * @brief Producer-side serializer for the PPP-RTK network joint solution.
 *
 * Sink A of the network product-emit path (W8): maps one epoch of the network's
 * estimated satellite-side solution (estimable satellite clock and code/phase
 * bias) into the existing SSR record structs and pushes them into a
 * @c SsrCorrectionStore through its push API, in the IGS-SSR convention that the
 * user consumer reads.  The existing user pipeline thereby serves as the
 * closed-loop integer-recovery validation harness: it tests whether the project's
 * own user engine recovers integers from the project's own products on a held-out
 * station.
 *
 * The emitter does not estimate; it serializes a @c NetworkSolutionEpoch that the
 * network estimator (W4) produces.  It does own the discontinuity bookkeeping,
 * which is stream state the estimator cannot know: it tracks the last emitted
 * phase bias per (sat, signal) and increments the @c discontinuity_counter on a
 * bias jump, or, through @c markDatumChange, on a global pivot or S-basis
 * re-declaration (wired in by W6b).  The consumer's @c noteSsrDiscontinuity ->
 * @c resetAmbiguity hook turns that increment into an ambiguity reset.
 *
 * It owns the analogous bookkeeping for satellite code biases (H1).  The
 * per-(constellation, signal) sum-to-zero datum re-references whenever the set of
 * satellites carrying a signal changes (rise/set), and the per-satellite
 * ionosphere-free pin re-references whenever that satellite's distinct-band set
 * changes; both step the emitted code biases.  The emitter detects both from the
 * epoch's own @c code_bias_m composition, so a replay reproduces the counters with
 * nothing extra serialized, as for phase.  It also honours the @c markDatumChange
 * forced bump (a clock-pivot handover re-references the whole shared solution).
 * By default code has no magnitude threshold: the deterministic set/band-change
 * detectors cover every gauge re-reference, and a code random-walk noise step is
 * not a datum discontinuity.  Code-bias values can still jump without a structural
 * trigger (weak-separation wander on thin rosters; a wire audit at 305 stations
 * counted 7.6k value jumps against 2.8k signalled), so an opt-in magnitude jump
 * test (@c network.code_bias_disc_jump_test, same threshold as phase) can be
 * layered on top to signal those moves.  The consumer's
 * @c getCodeBiasDiscontinuity -> @c reinflateCodeBiasVariance hook turns an
 * increment into a receiver code-bias re-inflation.
 *
 * The clock channel (A2) bumps by default only on forced datum changes (pivot
 * handover, retro-bump).  Under the sum-to-zero clock datum the gauge also
 * re-references whenever a constellation's set of clock satellites changes (the
 * survivors redistribute ∓ΔC0/N).  An opt-in set-change trigger
 * (@c network.clock_disc_on_set_change) detects that from the epoch's own clock
 * composition (replay-reproducible, like the code set trigger) and steps the
 * constellation's clock counters so that the datum move is signalled on the wire.
 *
 * Sink B (RTCM-4076 wire encoding) is deferred to Phase 2.
 *
 * Note: push into an owned (non-singleton) SsrCorrectionStore for the closed loop.
 * A producer does not share the global singleton store that it or its consumer
 * reads from, and tests do not modify global state.
 */

#include "products/PhaseStepClassTest.hpp"     // the step-class declaration (network.disc_step_class_pairs)
#include "products/PhaseVisibleStepTest.hpp"   // the phase-visible step test (network.disc_phase_visible)
#include "rtcm/RtcmTypes.hpp"   // GnssTime, SSR record structs

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class SsrCorrectionStore;

/// One satellite's estimable phase bias for a single signal (producer side).
struct NetworkPhaseBias {
    double bias_m{0.0};            ///< Estimable satellite phase bias (m); carries the pivot-receiver ambiguity lump (Odijk Eq. 4).
    bool   signal_integer{false}; ///< Bias preserves the integer ambiguity property (IGS-SSR).
    int    wide_lane_integer{0};  ///< Wide-lane integer indicator (2-bit, IGS-SSR).
    double sigma_m{0.0};          ///< Formal 1σ (m) of this phase-bias correction; 0 unless the driver populated it (network.emit_phase_bias_sigma, gauge-exact shared-cov diagonal) (B1b).
    double sigc_m{0.0};           ///< Formal 1σ (m) of the combined phase correction (clock + phase bias, cross-covariance included, no iono); 0 unless network.emit_combined_sigma populated it. Serialized as the P-record sigc= token; consumed through SsrPhaseBiasEntry::combined_sigma_m (M4, contract v2).
    /// Hold generation that this emitted value belongs to (
    /// @c network.emit_pb_hold).  0 means not held (the knob is off, or this key is
    /// outside the hold's scope), and the value is the estimator's own per-epoch
    /// estimate.  Nonzero means @ref bias_m was overwritten with the first value of
    /// that generation by @ref networkPbHold, which runs as the last writer before
    /// @ref NetworkProductEmitter::emitToStore, so the emitter's value-jump test and
    /// every downstream sink see the held value.  Stamped onto
    /// @c SsrPhaseBiasEntry::hold_generation and serialized as the P-record @c gen=
    /// token; a change of it is the declaration on which a consumer refloats.
    std::uint32_t hold_generation{0};
    /// Component of the chain datum that this satellite phase bias belongs to, as a
    /// P-record @c cmp=\<n\> token declares it (a truth wire carries @c cmp=1).  Two phase
    /// biases of one signal share an integer frame only when their components are equal,
    /// so a consumer pairs satellites within a component.  0 means the record carries
    /// no token (every wire written before the token existed, every golden wire).  Read
    /// by @ref NetworkProductReader and handed to the consumer's observe-mode integer
    /// search; the writer does not emit it and it is not stamped onto
    /// @c SsrPhaseBiasEntry, so the store and every pre-existing output are unchanged.
    std::uint32_t chain_component{0};
};

/// One satellite's estimated satellite-side solution for a single epoch.
struct NetworkSatSolution {
    std::string                             sat;            ///< RINEX sat ID ("G01", "E11").
    bool                                    has_clock{false};  ///< Whether @c clock_c0_m is valid.
    double                                  clock_c0_m{0.0}; ///< Estimable satellite clock C0 (m).
    double                                  clock_c1_m_s{0.0}; ///< Estimable satellite clock drift C1 (m/s); 0 unless network.clock_model is on (B3b: the two-state oscillator drift that the consumer extrapolates as c0+c1·dt).
    double                                  clock_ura_m{0.0}; ///< Per-satellite URA 1σ (m) = EWMA(σ̂0) × formal clock σ; 0 unless has_ura (B1).
    bool                                    has_ura{false};   ///< Whether clock_ura_m is populated (network.emit_ura on) (B1).
    /// Formal 1σ (m) of the combined code correction (clock + code bias at a
    /// reference signal, cross-covariance included); 0 unless
    /// network.emit_combined_sigma populated it.  Per satellite (the C record).
    /// Serialized as the C-record sigcc= token; stored in
    /// SsrClockCorrection::combined_code_sigma_m (M4, contract v2).
    double                                  sigcc_m{0.0};
    /// Correlation ρ between the combined code and combined phase corrections at the
    /// reference signal (both share the clock term, so typically 0.6–0.9); 0 unless
    /// populated.  Serialized as the C-record rho= token (M4, contract v2).
    double                                  rho{0.0};
    /// Level step of this satellite since its previous emission (m) (the
    /// producer's re-base was removed, so only a wire written with it carries a nonzero
    /// step).  The code-only level re-base moved its clock record and each of its
    /// phase-bias records by this amount, so the phase side that the consumer applies
    /// (-clock + phase bias) did not move.  Serialized as the C-record @c lvl= token
    /// (written only when nonzero, so an unarmed wire is byte-identical) and read back
    /// by NetworkProductReader, because the emitter's phase-bias value-jump test
    /// subtracts it from each phase-bias step before testing.  The producer and a
    /// replay use the same arithmetic, so a replay reproduces every declaration.  0
    /// unless the wire declares a step.
    double                                  level_step_m{0.0};
    /// Return plan R-A (removed from the producer; read from an old wire): per code-bias
    /// signal, the FOLLOW STEP of a free same-band column since this satellite's previous
    /// emission (m): the re-base moved that code-bias record by this amount together with
    /// the clock, so a consumer reading band 1 through it sees no level step.  Serialized
    /// as the B-record @c lvl= token (written only when nonzero) and removed from the
    /// emitter's code-bias value-jump test, in the producer and in a replay.  Empty unless
    /// the follow moved a column.
    std::map<std::string, double>           cb_level_step_m;
    /// Broadcast ephemeris version (IODE, in the IGS-SSR matching convention) that the
    /// estimable clock @c clock_c0_m is a delta against, threaded from the producer
    /// emission path in @c brdc_rts mode (RTF-2).  -1 means sp3 mode or
    /// unstamped: @c emitToStore then leaves @c SsrClockCorrection.iode = -1 (so
    /// @c getOrbitClock's clock gate inherits the orbit anchor, byte-identical)
    /// and the C record serializes no @c iode= field.  The SSR store owns the field,
    /// its serialization and the stamping; the producer emission path populates it.
    int                                     clock_iode{-1};
    /// IOD-SSR of the RTS orbit correction that this clock was estimated against
    /// (RTF-2).  This is an orbit coherence echo: broadcast radial-orbit error
    /// is absorbed 1:1 into the clock, so the consumer verifies that it applied the
    /// same orbit update.  -1 means unstamped (brdc-only replay, sp3).  The orbit echo
    /// is subordinate to the @c iode= anchor: it is serialized as the C-record
    /// @c oiod= field only when @c clock_iode >= 0 (so the parent @c iode= token is
    /// written) and this field is >= 0.  A brdc_rts producer always stamps
    /// @c clock_iode (conventions §2), so "@c clock_iode >= 0" holds whenever this is
    /// set; a value set without @c clock_iode is dropped from the wire without notice.
    int                                     orbit_iod_ssr{-1};
    /// SSR content epoch of that referenced RTS orbit, the other operand of the
    /// consumer orbit/clock age-skew budget (RTF-2).  {0,0} means
    /// unstamped.  Like @c orbit_iod_ssr it is subordinate to the @c iode= anchor:
    /// serialized as the C-record @c oepoch= field only when @c clock_iode >= 0 and
    /// week != 0.
    GnssTime                                orbit_epoch{0, 0.0};
    std::map<std::string, double>           code_bias_m;    ///< RINEX obs code -> satellite code bias (m).
    /// Subset of @ref code_bias_m whose signal is a same-band companion on the T_GD
    /// pin's second band and whose inter-signal correction was estimated (RF-0f,
    /// @c network.sat_isc_state): some station tracked both that signal and the
    /// pinned one on this satellite, so `ISC^s = b(companion) - b(pinned)` is
    /// determined by data.  Those records carry `isc=obs` on the wire.  A companion
    /// whose ISC had to be pinned at 0 is an assertion that a consumer cannot tell
    /// from an estimate by value, so it is withheld from @ref code_bias_m entirely
    /// rather than flagged.  Empty unless the knob is armed; with it off, every
    /// record is byte-identical.
    std::set<std::string>                   isc_observed;
    std::map<std::string, NetworkPhaseBias> phase_bias;     ///< RINEX obs code -> satellite phase bias.
};

/// One epoch of the network joint solution to serialize.
struct NetworkSolutionEpoch {
    GnssTime                        epoch_time{0, 0.0}; ///< SSR content reference epoch.
    /// Wall-clock time of generation (when the producer solved this epoch), distinct
    /// from @c epoch_time (the content reference) (RT0-4).  Left {0,0} unless the
    /// producer stamps it (network.emit_generation_time on); {0,0} means not stamped
    /// and serializes no G record, so the product is byte-identical.  A consumer
    /// measures product latency as (ingest wall-clock − generation_time).
    GnssTime                        generation_time{0, 0.0};
    /// Solution-generation id (RTA-3): a per-run marker (uint32 derived from the GPS
    /// time of the first emitted epoch, week·604800 + ⌊tow⌋) that distinguishes one
    /// producer datum realization from another across restarts.  0 means not stamped
    /// (network.emit_generation_id off): no @c I record is serialized and the store's
    /// generation field stays unset, so the product is byte-identical.  Unlike
    /// @c iod_ssr (a per-epoch increment, meaningless as a generation marker) this is
    /// constant for a run and deterministic under file replay (it is serialized per
    /// epoch), while a restarted live producer, which first emits at a later epoch,
    /// mints a different id.  A consumer treats a change of a present id as a global
    /// datum break.
    uint32_t                        generation_id{0};
    int                             iod_ssr{0};         ///< IOD-SSR stamped on every record this epoch.
    int                             provider_id{0};     ///< IGS-SSR provider id.
    int                             solution_id{0};     ///< IGS-SSR solution id.
    std::vector<NetworkSatSolution> sats;               ///< Per-satellite solutions.
};

/**
 * @brief The discontinuity counters one epoch of a product file carries
 *        (@c network.serialize_disc_counters, RTA-4): the post-emit counters of the
 *        producer's own emission.
 *
 * A file reader passes them to @ref NetworkProductEmitter::emitToStore, which takes
 * each value in place of its own recompute.  Keys use the emitter's internal form.
 */
struct FiledDiscCounters {
    std::map<std::string, int> clock;  ///< "sat"        -> clock disc counter (C field 5).
    std::map<std::string, int> code;   ///< "sat|signal" -> code disc counter  (B field 5).
    std::map<std::string, int> phase;  ///< "sat|signal" -> phase disc counter (P field 7).
    /** @brief Whether the record carries any serialized counter. @return True when at least one of the three maps is non-empty. */
    bool any() const { return !clock.empty() || !code.empty() || !phase.empty(); }
};

/**
 * @class NetworkProductEmitter
 * @brief Serializes a @c NetworkSolutionEpoch into a @c SsrCorrectionStore.
 */
class NetworkProductEmitter {
public:
    /// Default phase-bias jump (m) that counts as a discontinuity.  Single definition
    /// of the literal on which the driver's emitter construction and the serialized
    /// S-basis contract's @c disc_threshold_m must agree (R9).  0.05 m is well below a
    /// GPS L1 cycle (~0.19 m), so cycle-level bias jumps trip it and noise does not.
    static constexpr double kDefaultDiscThresholdM = 0.05;

    /// @brief Construct an emitter with empty stream state; every parameter is a
    ///        bookkeeping option whose default reproduces the legacy behaviour.
    ///
    /// @param disc_threshold_m Phase-bias jump (m) that counts as a discontinuity.
    ///        Defaults to @ref kDefaultDiscThresholdM.
    /// @param emit_clock_disc When true (A2), the emitter maintains a per-satellite
    ///        clock discontinuity counter (bumped on a scoped datum change for that
    ///        satellite's constellation) and stamps it on every pushed
    ///        @c SsrClockCorrection.  Default false leaves @c disc_counter at 0, so the
    ///        store is byte-identical to the pre-A2 store.
    /// @param staleness_horizon_s Staleness horizon in seconds of product time, never
    ///        wall-clock time, so that replay stays deterministic (RTA-11).  When > 0
    ///        the emitter tracks the last-emitted epoch time per (sat[,signal]) key.
    ///        A key that reappears after a longer gap is a return after a gap, not a
    ///        fault: its baseline is reseeded, and on the phase-bias and code-bias
    ///        channels the value-jump test and the structural set/band-change triggers
    ///        do not fire for it, because they would compare against stale state.  A
    ///        forced datum change (@c markDatumChange, @c markSatDiscontinuity) and,
    ///        on the clock channel, a clock set change still step the counter, because
    ///        a consumer that kept its state across the gap needs the reset.  The
    ///        counter value persists (it never resets), so a consumer that kept state
    ///        sees no change from the gap itself.  Default 0 keeps every baseline
    ///        indefinitely (byte-identical).  Like the other two parameters it travels
    ///        in the serialized S contract (@c stale=) for @c makeReplayEmitter.
    /// @param code_bias_jump_test When true (@c network.code_bias_disc_jump_test),
    ///        the code-bias counter also bumps on a fresh-baseline value jump beyond
    ///        @p disc_threshold_m (the phase jump test applied to code), on top of the
    ///        always-on structural set/band-change triggers.  It signals the
    ///        weak-separation value wander that the structural triggers cannot see.
    ///        Default false = structural triggers only (byte-identical).  Travels
    ///        in the S contract (@c cbjump=) for @c makeReplayEmitter.
    /// @param clock_disc_on_set_change When true
    ///        (@c network.clock_disc_on_set_change; requires the sum-to-zero clock
    ///        datum and @p emit_clock_disc), a change of a constellation's set of
    ///        clock satellites between consecutive emits steps that constellation's
    ///        clock counters.  Under Σδtˢ=0 a membership change is a datum
    ///        re-reference (the survivors redistribute) that is otherwise
    ///        unsignalled.  Detected from the emitted epoch itself
    ///        (replay-reproducible).  Default false (byte-identical).  Travels in
    ///        the S contract (@c clkset=) for @c makeReplayEmitter.
    /// @param disc_threshold_frac_cycles When > 0 (@c network.disc_threshold_frac_cycles),
    ///        the phase jump test (and, under @p code_bias_jump_test, the code jump
    ///        test) uses a per-(sat,signal) threshold of
    ///        @p disc_threshold_frac_cycles × λ(sat,signal) instead of the flat
    ///        @p disc_threshold_m.  The contract stays fixed (λ is a public, static band
    ///        identity, deterministic on producer and replay consumer alike), and the
    ///        gate becomes uniform in cycles rather than metres.  The recommended 0.25
    ///        gives L1/E1 4.76 cm (close to the legacy 0.05 m), L2 6.1 cm, L5/E5a
    ///        6.37 cm.  Default 0.0 = the legacy flat @p disc_threshold_m
    ///        (byte-identical).  Travels in the S contract (@c discfrac=) for
    ///        @c makeReplayEmitter.
    /// @param disc_mark_scope_channel When true (@c network.disc_mark_scope),
    ///        a per-satellite discontinuity mark bumps the generations of only the
    ///        channels that the marking site says moved (@ref markSatDiscontinuity's
    ///        @c channels argument), instead of the clock, code-bias and phase-bias
    ///        generations of that satellite alike.  A clock-only guard (the Q3 anchor
    ///        decouple, the CLK-R step test) then no longer declares a phase-bias step
    ///        that did not happen (CST §7.1: 117 of 390 declared consumer cell-epochs
    ///        had < 0.02 m of PB motion).  The emitter's own per-(sat, signal) jump
    ///        tests are unaffected and remain the backstop for a re-realization on a
    ///        channel that a mark did not name.
    ///        Default false = the legacy satellite-wide scope (byte-identical).
    ///        Travels in the S contract (@c discscope=) for @c makeReplayEmitter.
    /// @param disc_phase_visible_min_sats When > 0 (@c network.disc_phase_visible), the
    ///        phase value test declares a step on what a user of between-satellite
    ///        differences sees: the change of the phase-visible correction (clock less
    ///        phase bias) less the median of that change over the satellites of the same
    ///        constellation and signal, against the same threshold (see
    ///        products/PhaseVisibleStepTest.hpp).  The value is the minimum number of
    ///        voting satellites for the median (@ref kPhaseVisibleMinSats when armed); a key
    ///        without a clock in this or its previous emission keeps the legacy test.
    ///        Default 0 = the legacy test on the phase bias alone (byte-identical).
    ///        Travels in the S contract (@c discpv=) for @c makeReplayEmitter.
    explicit NetworkProductEmitter(double disc_threshold_m = kDefaultDiscThresholdM,
                                   bool emit_clock_disc = false,
                                   double staleness_horizon_s = 0.0,
                                   bool code_bias_jump_test = false,
                                   bool clock_disc_on_set_change = false,
                                   double disc_threshold_frac_cycles = 0.0,
                                   bool disc_mark_scope_channel = false,
                                   int disc_phase_visible_min_sats = 0);

    /// Product channels that a per-satellite discontinuity mark applies to.
    /// A bit mask, so a marking site that moves two channels names both, and two
    /// sites marking the same satellite in one epoch are OR-ed together.
    enum DiscChannel : unsigned {
        kDiscChanClock     = 1u << 0,  ///< The satellite clock generation (C record).
        kDiscChanCodeBias  = 1u << 1,  ///< The satellite code-bias generations (B records).
        kDiscChanPhaseBias = 1u << 2,  ///< The satellite phase-bias generations (P records).
        /// Every channel: the legacy satellite-wide scope, and what an un-migrated
        /// caller (including a product replay, whose @c R record carries no channel
        /// set) asks for by omitting the argument.
        kDiscChanAll = kDiscChanClock | kDiscChanCodeBias | kDiscChanPhaseBias
    };

    /// One mark that this emit did not apply to a channel because the mark's scope
    /// excluded it: invariant #7's identity for the decoupling edge.
    struct MarkScopeEvent {
        std::string sat;              ///< RINEX satellite id.
        unsigned    marked{0};        ///< The channel mask the marking sites asked for.
        DiscChannel suppressed{kDiscChanClock};  ///< The channel that was not bumped.
    };

    /// @brief Suppressed marks of the last @ref emitToStore call, cleared at the top of every
    /// emit.  Always empty under the legacy satellite scope, so a run with
    /// the knob off allocates nothing and the driver's drain is a no-op.
    ///
    /// @return One row per (satellite, channel) whose mark was withheld; empty when nothing was
    ///         suppressed.  A reference into the emitter, valid until the next emit.
    const std::vector<MarkScopeEvent>& markScopeEvents() const { return mark_scope_events_; }

    /// Number of marks this emit applied to each channel, indexed by
    /// @ref DiscChannel (clock / code-bias / phase-bias).  Cleared per emit;
    /// always zero under the legacy satellite scope (nothing is counted there).
    struct MarkChannelTally {
        long long clk{0};   ///< Satellite-epochs whose clock generation a mark bumped.
        long long cb{0};    ///< (sat, signal) code-bias cells a mark bumped.
        long long pb{0};    ///< (sat, signal) phase-bias cells a mark bumped.
    };
    /// @brief Applied-mark tally of the last @ref emitToStore call.
    /// @return Counts of marks applied per channel; all zero under the legacy satellite scope.
    ///         A reference into the emitter, valid until the next emit.
    const MarkChannelTally& markChannelTally() const { return mark_channel_tally_; }

    /// @brief Whether the per-channel mark scope is armed on this emitter.
    /// @return True when constructed with @c disc_mark_scope_channel; false is the legacy
    ///         satellite-wide scope.
    bool discMarkScopeChannel() const { return disc_mark_scope_channel_; }

    /// @brief The phase-visible step test's minimum voter count.
    /// @return 0 when the legacy phase test is in force (@c network.disc_phase_visible off),
    ///         else the minimum number of voting satellites for the median.
    int discPhaseVisibleMinSats() const { return disc_pv_min_sats_; }

    /**
     * @brief Arm the step-class declaration (@c network.disc_step_class_pairs).
     *
     * At every emission each family's satellites have their phase step classified against
     * their previous emission (products/PhaseStepClassTest.hpp), and the signals of a whole
     * or fraction step are declared on the phase channel in place of the value test; a
     * classified key whose step is quiet or ionosphere-like is not declared.  A key of no
     * family, or not classified at an emission (no previous emission of the stream within
     * 45 s, the satellite absent from it, a code or phase bias or the clock missing, the
     * family below @ref kStepClassMinSats), keeps the value test.  Forced marks are
     * unchanged.  The baselines are not carried across a warm resume: the first emission
     * after a restore re-seeds them.
     *
     * @param pairs   Families, "<constellation>:<phase signal>/<phase signal>" ("E:L1X/L5X");
     *                each family's code signals are its phase signals with L read as C.
     *                Empty disarms the test.
     * @param tol_cyc The class tolerance (cycles, in (0, 0.5)).
     * @throws std::invalid_argument on a malformed entry, a band without a nominal frequency
     *         or a tolerance out of range.
     */
    void setStepClassTest(const std::vector<std::string>& pairs, double tol_cyc);
    /// @brief Whether the step-class declaration is armed.
    /// @return True when @ref setStepClassTest named at least one family.
    bool stepClassArmed() const { return !sc_families_.empty(); }
    /// @brief The step-class declaration's tolerance.
    /// @return Cycles.
    double stepClassTolCyc() const { return sc_tol_cyc_; }
    /// @brief The step-class declaration's run-end counters (every family and emission so far).
    /// @return The counters.
    const StepClassCounters& stepClassCounters() const { return sc_counters_; }
    /// @brief Phase-key increments the step-class declaration raised.
    /// @return Count over emissions.
    long long stepClassKeysStepped() const { return sc_keys_stepped_; }
    /// @brief Classified keys the value test would have declared and the class test did not.
    /// @return Count over emissions.
    long long stepClassValueOnly() const { return sc_value_only_; }
    /// @brief Classified keys the class test declared and the value test would not have.
    /// @return Count over emissions.
    long long stepClassClassOnly() const { return sc_class_only_; }

    /**
     * @brief Declare satellite signals the chain released at this epoch (the step declaration's release
     *        increments).
     *
     * Under the step-class declaration (@ref setStepClassTest) every key named here steps its phase counter
     * at the next emission that carries it, in addition to the class rule: the chain freed the key's level,
     * and the freed level re-converges over the following epochs in steps each under the class tolerance.  A
     * key absent from that emission stays pending until it is emitted; one emitted without a baseline (a
     * first sighting) is consumed without an increment.  Ignored while the declaration is not armed.  The
     * pending marks are not carried across a warm resume (as the class baselines).
     *
     * @param keys (satellite, phase signal) pairs, e.g. ("E14", "L1C").
     */
    void markReleasedSignals(const std::set<std::pair<std::string, std::string>>& keys);
    /// @brief Release marks received (the chain's declared keys, every emission so far).
    /// @return Count over the run.
    long long stepClassReleaseMarks() const { return sc_release_marks_; }
    /// @brief Release marks consumed on an emitted key with a baseline (each one an increment).
    /// @return Count over the run.
    long long stepClassReleaseLanded() const { return sc_release_landed_; }
    /// @brief Landed release marks whose key neither a forced mark nor the class or value test stepped.
    /// @return Count over the run.
    long long stepClassReleaseOnly() const { return sc_release_only_; }
    /// @brief Release marks consumed on an emitted key without a baseline (no increment).
    /// @return Count over the run.
    long long stepClassReleaseNoBaseline() const { return sc_release_no_baseline_; }
    /// @brief Release marks waiting for their key's next emission.
    /// @return Count now.
    std::size_t stepClassReleasePending() const { return sc_release_pending_.size(); }

    /**
     * @brief Arm the unsettled steps (@c network.ring_fix.unsettled_steps).
     *
     * For each family and satellite at every emission: while the family's two phase signals are not both flagged
     * settled (signal_integer on both P records), both signals step their phase counters; at the first emission at
     * which both are flagged after one at which they were not, they step once more; while both stay flagged only the
     * other rules step them.  A family signal absent from the emission counts as not flagged; a key without a
     * baseline (a first sighting) does not step.  Signals of no family are untouched.  The previous emission's state
     * is not carried across a warm resume: an unknown previous state counts as not settled, so a family flagged at
     * the first emission after a restore takes the settling step.
     *
     * @param families "<constellation>:<phase signal>/<phase signal>" ("E:L1X/L5X"), the ring fix's held families.
     * @throws std::invalid_argument on a malformed family.
     */
    void setUnsettledSteps(const std::vector<std::string>& families);
    /**
     * @brief Arms the unsettled steps of the signals of no held family (network.ring_fix.unsettled_steps_unheld).
     *
     * With @ref setUnsettledSteps armed, a phase signal that belongs to no family it named (for the satellite's
     * constellation) is never settled, so its phase counter steps at every emission that finds a baseline, filled
     * copies included.  Its steps are counted under "<constellation>:no held family" in
     * @ref unsettledStepCounts.  Without @ref setUnsettledSteps nothing happens.
     *
     * @param on True to arm.
     */
    void setUnsettledStepsUnheld(bool on) { us_unheld_ = on; }
    /**
     * @brief The phase-family members (network.phase_families; card NRF section 109.1, amendment 3): a member signal's
     *        phase-bias entry repeats its identity's discontinuity counter and integer flag at every emission.
     *
     * The values are copied into the epoch by the network before the push; this keeps the declarations equal too,
     * whatever per-key rule (value test, step class, phase-visible test, release, unsettled steps, continuity guard)
     * stepped or lowered the identity.  Empty (default): nothing changes.
     * @param member_identity (constellation letter, member phase signal) -> the identity phase signal ("E", "L1X") -> "L1C".
     */
    void setPhaseFamilyMembers(std::map<std::pair<char, std::string>, std::string> member_identity) {
        pf_member_of_ = std::move(member_identity);
    }
    /// @brief Whether the unsettled steps are armed.
    /// @return True when @ref setUnsettledSteps named at least one family.
    bool unsettledStepsArmed() const { return !us_families_.empty(); }
    /// @brief The phase-key steps the unsettled steps raised, per family: (while unsettled, at the settling).
    /// @return Family name ("E:L1X/L5X") -> the two counts over the run.
    const std::map<std::string, std::pair<long long, long long>>& unsettledStepCounts() const { return us_counts_; }

    /**
     * @brief Arm the stream's continuity guard (@c network.ring_fix.continuity_guard_cyc).
     *
     * Needs @ref setUnsettledSteps (its families are the guarded families).  At every emission, per family and
     * satellite with both phase signals, both code biases of the same bands (C for L in the signal code) and a clock,
     * the satellite's level in narrow-lane cycles is formed from the emitted values,
     * E_b = (c0 - pb_b + mu_b x) / lambda_b with x = ((c0 - cb_1) - (c0 - cb_2)) / (1 - gamma) and mu_1 = 1,
     * mu_2 = gamma, combined as (f1 E_1 - f2 E_2) / (f1 - f2) after the family's datum is removed (the running sum of
     * the median epoch-to-epoch step of E_1 and of E_2 over the satellites at both emissions, 4 or more).  A run is a
     * sequence of emissions with both flags up, neither counter stepped and no hold; after @p min_run emissions of a
     * run, a level more than @p thr_cyc away from its value at the run's start trips the guard: both flags are lowered
     * at that emission and the next @p hold - 1, and both counters step at that emission; the unsettled steps then
     * step them while lowered and once more when the flags rise.  The state is not carried across a warm resume (a
     * restored emitter starts every run afresh).
     *
     * @param thr_cyc Threshold in narrow-lane cycles, > 0.
     * @param hold    Emissions the flags stay lowered, >= 1.
     * @param min_run Emissions of a run before its level is compared, >= 1.
     * @throws std::invalid_argument when the unsettled steps are not armed or a value is out of range.
     */
    void setContinuityGuard(double thr_cyc, int hold, int min_run);
    /// @brief Whether the continuity guard is armed.
    /// @return True after @ref setContinuityGuard.
    bool continuityGuardArmed() const { return cg_thr_cyc_ > 0.0; }
    /// @brief Trips of the continuity guard, per family.
    /// @return Family name ("E:L1X/L5X") -> trips over the run.
    const std::map<std::string, long long>& continuityGuardTrips() const { return cg_trips_; }
    /// @brief Phase-key emissions whose flag the continuity guard lowered, over the run.
    /// @return The count.
    long long continuityGuardLoweredKeys() const { return cg_lowered_keys_; }
    /**
     * @brief Lower, in @p sol, the flags the continuity guard lowered at the last @ref emitToStore.
     *
     * The store's records carry the guard's flags from @ref emitToStore; the product file is written from the
     * solution record, so the emit seam applies them to @p sol after the emission and before the file write, and the
     * file then says what the store says.  Nothing happens when the guard is off or lowered nothing.
     *
     * @param sol The epoch's solution as emitted (its P-record flags are lowered in place).
     */
    void applyContinuityGuardFlags(NetworkSolutionEpoch& sol) const;

    /**
     * @brief Both edges of the phase-visible step test (standing invariant 7), counted over
     *        the emitter's life.  All zero when the test is off.
     */
    struct PhaseVisibleEdges {
        long long first_records{0};      ///< Keys emitted with a clock and no phase-visible baseline: seeded, not tested.
        long long returns_fresh{0};      ///< Keys tested against a baseline older than the previous emission (not voting).
        long long returns_stale{0};      ///< Keys whose baseline is past the staleness horizon: re-seeded, not tested.
        long long forced{0};             ///< Keys under a forced mark: declared by the mark, not voting.
        long long no_clock{0};           ///< Keys emitted without a clock, or after one: the legacy test applies.
        long long vote_entries{0};       ///< Keys voting this emission that did not vote at the previous one.
        long long vote_exits{0};         ///< Keys that voted at the previous emission and do not vote at this one.
        long long below_min_entries{0};  ///< Groups below the minimum count this emission and not at the previous one.
        long long below_min_exits{0};    ///< Groups below the minimum count at the previous emission and not at this one.
        long long legacy_would_step{0};  ///< Tested keys the legacy test (the phase bias alone) would have declared.
        long long stepped_not_legacy{0}; ///< Tested keys declared here that the legacy test would not have declared.
    };

    /// @brief Counters of the phase-visible step test (groups, medians, declarations).
    /// @return A reference into the emitter; all zero when the test is off.
    const PhaseVisibleCounters& phaseVisibleCounters() const { return pv_counters_; }

    /// @brief Edge counters of the phase-visible step test.
    /// @return A reference into the emitter; all zero when the test is off.
    const PhaseVisibleEdges& phaseVisibleEdges() const { return pv_edges_; }

    /**
     * @brief Channel mask of the marks pending for one satellite.
     *
     * A pure read of the same two sources that @c emitToStore's internal
     * @c constForced / @c satForced lambdas consult, without their side effects
     * (no tally, no @ref MarkScopeEvent row).  A caller can therefore ask what is
     * about to be declared for this satellite before the emit without changing the
     * MS census by a single count.
     *
     * @param sat RINEX satellite id ("G01").
     * @return 0 when nothing is pending; otherwise the OR of @ref DiscChannel bits.
     *         A constellation-scoped @ref markDatumChange (or the global bump)
     *         contributes @ref kDiscChanAll because it re-declares every channel of
     *         every satellite of that constellation; a @ref markSatDiscontinuity
     *         contributes @ref kDiscChanAll under the legacy @c sat scope and the
     *         mask the marking sites named under the @c channel scope.
     *
     * @note Valid between the marking calls of an epoch and the @ref emitToStore
     *       that consumes them; @ref emitToStore clears the pending state.
     */
    unsigned pendingMarkChannels(const std::string& sat) const;

    /// @brief The jump-test threshold (m) for a given (sat, signal): frac·λ when the
    /// fractional-cycle mode is armed and λ resolves, else the flat metre gate.  Public
    /// so the hold can read an estimator step against the bar the emitter applies to
    /// the wire (the smoothed-hold block member's unmarked steps).
    ///
    /// @param sat    RINEX satellite id ("G01"); selects the constellation for the band lookup.
    /// @param signal RINEX observation code of the signal ("L1C").
    /// @return Threshold in metres.  The flat @c disc_threshold_m when the fractional-cycle
    ///         mode is off or the signal's nominal wavelength is unknown (0); otherwise
    ///         @c disc_threshold_frac_cycles times the nominal wavelength in metres.
    double discThresholdM(const std::string& sat, const std::string& signal) const;

    /// @brief Stable lowercase name of one channel ("clk" / "cb" / "pb"), for trace events
    /// and tests.
    ///
    /// @param c A single channel bit.
    /// @return Pointer to a string literal (static storage, never null): "clk", "cb" or "pb"
    ///         for the three single channels, "?" for any other value (including
    ///         @ref kDiscChanAll).
    static const char* discChannelName(DiscChannel c);

    /// @brief Number of phase-bias value-jump tests whose raw step exceeded the bar
    ///        only because of the satellite's declared level step
    ///        (@ref NetworkSatSolution::level_step_m, the C-record lvl= token), and
    ///        which therefore did not declare once the step was removed.
    /// @return the run count (0 unless the wire declares a level step).
    long long clvJumpExcluded() const { return clv_jump_excluded_; }

    /// @brief Return plan R-A: code-bias value-jump tests whose raw step exceeded
    ///        the bar only because of the column's declared follow step
    ///        (@ref NetworkSatSolution::cb_level_step_m, the B-record lvl= token), and
    ///        which the step's removal therefore did not declare.
    /// @return the run count (0 unless the wire declares a follow step).
    long long clvCbJumpExcluded() const { return clv_cb_jump_excluded_; }

    /// @brief Serialize @p sol into @p store (Sink A: in-process, no wire bytes).
    ///
    /// @param sol Epoch of the network joint solution to serialize; read only.  Its per-satellite
    ///        clock, code-bias and phase-bias entries become SSR records, stamped with the
    ///        emitter's current discontinuity counters.
    /// @param[in,out] store Destination store that receives the pushed clock, URA, code-bias and
    ///        phase-bias records (an owned, non-singleton instance in the closed loop).
    /// @param filed Optional counters a product file carries for this epoch (a file reader, see
    ///        NetworkProductReader::replay).  For every key present there, the emitter first runs
    ///        its own rules (baselines, value tests, marks, the member copy and the guard all
    ///        advance as without it), then takes the file's value as its counter before the record
    ///        is pushed; each value that differs from its own is listed in
    ///        @ref lastFiledDisagreements.  The epoch is emitted once, so the declared level steps,
    ///        the value tests and every run-end counter run once per epoch.  nullptr (the producer,
    ///        and a file without counters): the counters are the emitter's own.
    void emitToStore(const NetworkSolutionEpoch& sol, SsrCorrectionStore& store,
                     const FiledDiscCounters* filed = nullptr);

    /// @brief How a file counter that differs from the emitter's own recompute is classed, from
    ///        what the record and the emitter hold.  Phase values are classed by direction and by
    ///        the record's @c signal_integer flag: the file stepped while the flag is down (the
    ///        producer's unsettled steps), at the emission where the flag rose (the settling step),
    ///        or while the flag is up (a step-class declaration, a family member's copy, a guard
    ///        trip or a release mark); or the replay stepped where the file did not (its value test
    ///        against a rule the producer replaced).
    enum class FiledDisagreementClass : unsigned char {
        PhaseFlagDown = 0,   ///< Phase, file ahead, @c signal_integer 0 on this record.
        PhaseFlagRise,       ///< Phase, file ahead, the flag rose at this emission.
        PhaseFlagUp,         ///< Phase, file ahead, the flag up at this and the previous emission.
        PhaseReplayAhead,    ///< Phase, the replay stepped more than the file.
        Code,                ///< Code-bias counter.
        Clock,               ///< Clock counter.
    };
    /// @brief Number of @ref FiledDisagreementClass values.
    static constexpr std::size_t kFiledDisagreementClasses = 6;

    /// @brief Stable lowercase name of a class, for log lines and tests.
    /// @param c The class.
    /// @return Pointer to a string literal (never null).
    static const char* filedDisagreementClassName(FiledDisagreementClass c);

    /// @brief One file counter that differed from the emitter's own recompute.
    struct FiledDisagreement {
        FiledDisagreementClass cls;    ///< Its class.
        std::string            key;    ///< "sat|signal" (code, phase) or "sat" (clock).
        int                    filed;  ///< The file's value, which the emitter took.
        int                    own;    ///< The emitter's own recompute.
    };

    /// @brief The disagreements of the last @ref emitToStore call, in emission order; empty when
    ///        it had no file counters or every one agreed.
    /// @return Reference valid until the next @ref emitToStore call.
    const std::vector<FiledDisagreement>& lastFiledDisagreements() const { return filed_disagreements_; }

    /// @brief Run totals of @ref lastFiledDisagreements by class (index = the class value).
    /// @return One count per class.
    const std::array<long long, kFiledDisagreementClasses>& filedDisagreementTotals() const {
        return filed_disagreement_totals_;
    }

    /// @brief Force a discontinuity bump at the next emit, scoped to the given constellation
    /// chars (e.g. {'G','E'}): a per-constellation pivot handover or S-basis
    /// re-declaration.  An empty set forces every constellation (the
    /// backward-compatible global bump, matching the pre-A2 single-flag behavior).
    ///
    /// Pending semantics (RT0-5 seam): a constellation's flag is consumed only by an
    /// emit that carries at least one of its satellites.  If the next emitted epoch
    /// has no satellites of a flagged constellation (e.g. the RT0-5 clock-runaway
    /// quarantine stripped them), the flag stays pending and the bump lands at the
    /// first emit where that constellation reappears.  The discontinuity signal
    /// describes the last emitted product, so a quarantine that coincides with a datum
    /// change cannot drop it.  Repeated marks while pending coalesce into a single
    /// bump.  Deterministic on replay: the pending state is a pure function of the
    /// emitted epoch sequence and the markDatumChange calls, both of which the product
    /// file reproduces (E-line datum_consts on the trigger epoch, and the same pruned
    /// epochs).
    ///
    /// @param constellations RINEX constellation letters to bump ('G', 'E', 'C', ...).  The
    ///        default empty set forces every constellation.
    void markDatumChange(const std::set<char>& constellations = {});

    /// @brief Force a discontinuity bump at the next emit, scoped to the given satellite ids
    /// (e.g. {"G13","E09"}).  This is the B1 retro-bump: a post-emit QC tier flagged
    /// these satellites' corrections as contaminated, so the consumer must reset the
    /// ambiguity that integrated the one bad epoch.  Unlike @c markDatumChange (a
    /// whole-constellation datum re-reference) this steps only the named satellites'
    /// phase, code and clock counters, a per-satellite reset rather than a reset of
    /// every satellite in the constellation (P1-3).  Applies to the next emit only.
    ///
    /// @param satellites The satellites to mark.
    /// @param channels   Which product channels this mark reports, a bit-or of
    ///        @ref DiscChannel.  Honoured only when the emitter was
    ///        constructed with @c disc_mark_scope_channel; under the legacy satellite
    ///        scope every mark bumps all three generations, so passing a narrower mask
    ///        is byte-identical there.  The default @ref kDiscChanAll is what an
    ///        un-migrated caller, and a product replay (whose @c R record carries no
    ///        channel set), asks for.  Two sites marking the same satellite in one
    ///        epoch OR their masks.
    void markSatDiscontinuity(const std::set<std::string>& satellites,
                              unsigned channels = kDiscChanAll);

    /// One constellation's tally in a @ref GnssProcessor::NetworkProductEmitter::pbSeamCensus "pbSeamCensus": how many of its phase-bias
    /// keys the jump test would examine this epoch, and how many of those exceed
    /// @c discThresholdM (RS-4, SPEC_event_harness §21.9).
    struct PbSeamConstTally {
        long long keys_tested{0};   ///< Keys carrying a non-stale baseline (the jump test's own population).
        long long keys_over{0};     ///< Of those, keys whose |bias − baseline| exceeds discThresholdM.
        double    max_abs_delta_m{0.0}; ///< Largest |bias − baseline| over the tested keys (m); the event's statistic.
    };

    /**
     * @brief Census of this epoch's emitted phase biases against the current
     *        per-key baselines, without mutating any emitter state (RS-4).
     *
     * Read-only twin of the jump test inside @ref GnssProcessor::NetworkProductEmitter::emitToStore "emitToStore": same population (a key
     * with a baseline that is not stale under the staleness horizon), same threshold
     * (@c discThresholdM), same key spelling.  Call it before @ref emitToStore; after
     * that call every baseline holds this epoch's value and the census is trivially
     * empty.
     *
     * It serves the warm-resume seam: on a restored emitter the baselines are the
     * previous run's emitted values, so a level re-realization at the first
     * post-resume emit appears here as a large tested-key fraction, which
     * @c network.resume_pb_seam_mark turns into a @c markDatumChange.  Const and free
     * of side effects, so a run that never calls it is byte-identical.
     *
     * @param sol The epoch about to be emitted (its @c phase_bias maps are read).
     * @return Per-constellation tallies, keyed by the satellite id's leading char.
     *         A constellation with no baselined key at all is absent from the map.
     */
    std::map<char, PbSeamConstTally>
    pbSeamCensus(const NetworkSolutionEpoch& sol) const;

    /// @brief Current phase-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    ///
    /// @param sat    RINEX satellite id ("G01").
    /// @param signal RINEX observation code of the signal ("L1C").
    /// @return The counter stamped on that key's phase-bias records; increments on each declared
    ///         discontinuity and never resets.  0 when the key has not been tracked.
    int discontinuityCounter(const std::string& sat, const std::string& signal) const;

    /// @brief Current code-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    ///
    /// @param sat    RINEX satellite id ("G01").
    /// @param signal RINEX observation code of the signal ("C1C").
    /// @return The counter stamped on that key's code-bias records; increments on each declared
    ///         discontinuity and never resets.  0 when the key has not been tracked.
    int codeBiasDiscontinuityCounter(const std::string& sat, const std::string& signal) const;

    /// @brief Current clock discontinuity counter for a satellite, or 0 if untracked.
    /// Always 0 unless the emitter was constructed with @c emit_clock_disc (A2).
    ///
    /// @param sat RINEX satellite id ("G01").
    /// @return The counter stamped on that satellite's clock records; increments on each declared
    ///         clock discontinuity and never resets.  0 when untracked.
    int clockDiscontinuityCounter(const std::string& sat) const;

    /**
     * @brief Whether this phase-bias key is a first sighting.
     *
     * The emitter's own test, @c last_phase_bias_.find(key) == end(), exposed so it
     * can be compared with the arc ledger, which knows whether the family is
     * returning.  A first sighting emits no discontinuity and re-seeds the baseline,
     * so a return read as a first sighting is a re-coupling edge that the consumer is
     * never told about (standing invariant 7).
     *
     * Call before @ref emitToStore.  That call overwrites the baselines with this
     * epoch's values, after which every key answers false.
     *
     * @param sat    Satellite id.
     * @param signal RINEX signal.
     * @return true when the emitter has no baseline for this key.
     */
    bool isPhaseFirstSighting(const std::string& sat, const std::string& signal) const;

    /// @brief Code-bias twin of @ref isPhaseFirstSighting.
    ///
    /// @param sat    Satellite id.
    /// @param signal RINEX signal.
    /// @return true when the emitter has no last-emitted code-bias baseline for this key.
    bool isCodeFirstSighting(const std::string& sat, const std::string& signal) const;

    /// @brief Clock twin of @ref isPhaseFirstSighting.  Always true
    /// unless the emitter was constructed with @c emit_clock_disc (A2), which is what
    /// fills @c seen_clock_sats_.
    ///
    /// @param sat Satellite id.
    /// @return true when this satellite's clock has not been emitted yet (always true when
    ///         clock discontinuity tracking is off).
    bool isClockFirstSighting(const std::string& sat) const;

    /**
     * @brief Adopt serialized discontinuity-counter values into this emitter's
     *        stream state, for late-joining and tail readers (RTA-4).
     *
     * When a product file carries the optional per-record counter fields
     * (@c network.serialize_disc_counters) and the reader's replay recompute
     * disagrees with them (the expected case for a reader that starts mid-file,
     * where every key is a first sighting and recomputes to 0), the reader calls this
     * to overwrite the emitter's counters with the file's authoritative post-emit
     * values, then re-emits the epoch so that the store records carry them.  Keys use
     * the emitter's internal "sat|signal" form (@c clock keyed by bare sat id).
     * Only the counters are touched: the preceding @c emitToStore already seeded the
     * bias baselines, first-sighting markers and satellite-set trackers, so later
     * epochs recompute increments on top of the adopted values as the live producer
     * would.
     *
     * @param phase Map "sat|signal" → phase-bias discontinuity counter.
     * @param code  Map "sat|signal" → code-bias discontinuity counter.
     * @param clock Map "sat"        → clock discontinuity counter.
     */
    void adoptDiscontinuityCounters(const std::map<std::string, int>& phase,
                                    const std::map<std::string, int>& code,
                                    const std::map<std::string, int>& clock);

    /**
     * @brief Serialize the emitter's entire cross-epoch stream state to @p os as
     *        self-terminated, line-oriented text (RTB-7).
     *
     * Holds everything a warm-resumed producer needs to continue the discontinuity
     * bookkeeping where the stopped process left it: the phase/code/clock
     * discontinuity counters, the last-value baselines
     * (@c last_phase_bias_ / @c last_code_bias_), the first-sighting sets
     * (@c seen_clock_sats_ / @c prev_code_satset_ / @c prev_band_tuple_, and
     * @c prev_clock_satset_ when armed), the RTA-11 last-seen timestamp maps, and the
     * RT0-5 pending forced-bump state (@c force_all_bump_ / @c forced_bump_consts_ /
     * @c forced_bump_sats_, with each mark's channel mask under the MS channel
     * scope).  Restoring the counters without the baselines, first-sighting sets and
     * pending flags would emit wrong counters after a resume (spurious first-sighting
     * resets, lost deferred datum bumps), which the consumer could not detect as a
     * datum break, so the state is written and restored as a whole.
     *
     * Format: an @c EMITTER header carrying the construction parameters that a
     * restore must match (a restore into a differently configured emitter fails):
     * @c v1 holds the disc threshold, the clock-disc flag and the staleness horizon;
     * @c v2 adds the code-bias jump-test and clock set-change flags and is written
     * when either is armed; @c v3 adds the fractional-cycle threshold and is written
     * when that is armed.  Then one line per map/set entry in deterministic key
     * order, and an @c EEND terminator.  Doubles use fmt's shortest round-trippable
     * formatting (the @c NetworkProductIO convention), so a parse recovers the exact
     * value.
     *
     * The emitter knows nothing about files: the caller (the RTB-7 snapshot
     * module) owns framing, storage and atomicity.
     *
     * @param os Destination stream (any @c std::ostream; no flush performed).
     */
    void serializeState(std::ostream& os) const;

    /**
     * @brief Restore stream state previously written by @ref serializeState,
     *        replacing this emitter's entire cross-epoch state (counters, baselines,
     *        first-sighting sets, RTA-11 last-seen stamps, RT0-5/B1 pending
     *        forced-bump flags) (RTB-7).
     *
     * All-or-nothing: the input is parsed into staging state first, and only a
     * complete, well-formed section (header through @c EEND) whose construction
     * parameters match this emitter's is committed.  On any failure (truncation,
     * malformed field, unknown tag, or a parameter mismatch, meaning the emitter was
     * constructed from a different config than the snapshotting producer's) the
     * emitter is left untouched and false is returned, which the caller treats as
     * the cold-start signal.
     *
     * Distinct from @ref adoptDiscontinuityCounters (RTA-4), which overwrites only
     * counter values on top of live-seeded baselines for a mid-file product reader;
     * this is a full state transplant for a producer resume.
     *
     * @param is Source stream positioned at the @c EMITTER header line.
     * @return True when the state was fully restored.
     */
    bool restoreState(std::istream& is);

    /**
     * @brief Write the emitter's state that depends on the network's held values, for the snapshot's chain memory
     *        section (network.state_snapshot_chain_capture): the step declaration's baselines, its last emission and
     *        its pending release marks, the unsettled steps' last flags, and the continuity guard's levels, datums,
     *        runs and holds, one line each (tags @c E*).  The run counters are not written.  The rest of the
     *        emitter's cross-epoch state travels in the emitter section (@ref serializeState).
     * @param os The section being written.
     */
    void writeHeldValueState(std::ostream& os) const;

    /**
     * @brief Read one line of the held-value state back (network.resume_chain_restore).
     * @param f The line's comma-separated fields, the tag first.
     * @return True when the line is one of the emitter's and was read; false for another tag or a malformed line.
     */
    bool readHeldValueStateLine(const std::vector<std::string>& f);

private:
    double disc_threshold_m_;
    /// When > 0, the per-(sat,signal) jump threshold is disc_threshold_frac_cycles_ ×
    /// λ(sat,signal) (fixed contract, uniform in cycles).  0 = legacy flat metres (H3).
    double disc_threshold_frac_cycles_{0.0};
    /// Bump only the channels that a mark named.  Default false = the
    /// legacy satellite-wide scope (byte-identical).  Travels in the S contract
    /// (@c discscope=).  Declared here so that the ctor's init list stays in
    /// declaration order.
    bool   disc_mark_scope_channel_{false};
    long long clv_jump_excluded_{0};   ///< See clvJumpExcluded().
    /// File counters that differed from this emitter's recompute at the last emitToStore (file readers only).
    std::vector<FiledDisagreement> filed_disagreements_;
    std::array<long long, kFiledDisagreementClasses> filed_disagreement_totals_{};   ///< Run totals by class.
    /// "sat|signal" -> the phase record's signal_integer at the key's previous emission with a file counter; it
    /// classes a flag rise.  File readers only; not part of the serialized stream state.
    std::map<std::string, bool> filed_prev_flag_;
    /// Take the file's counter @p filed as @p own, listing a disagreement under @p cls first.
    void takeFiledCounter_(FiledDisagreementClass cls, const std::string& key, int filed, int& own);
    long long clv_cb_jump_excluded_{0};   ///< return plan R-A: see clvCbJumpExcluded()
    bool   emit_clock_disc_{false};       ///< Maintain and stamp the clock disc counter (A2).
    /// Staleness horizon (s, product time; 0 = off) (RTA-11).  With it on, the
    /// last_seen maps below record each key's last-emitted epoch time.  A key that
    /// returns after a longer gap has its baseline reseeded, and the phase and code
    /// value-jump tests and the code structural triggers do not fire for it; a forced
    /// datum change (and, on the clock channel, a clock set change) still steps the
    /// counter.  The maps are populated only when the horizon is armed, so with it
    /// off the emitter carries no extra state.
    double staleness_horizon_s_{0.0};
    std::map<std::string, double> phase_last_seen_s_;  ///< "sat|signal" -> last emit product time (s).
    std::map<std::string, double> code_last_seen_s_;   ///< "sat|signal" -> last emit product time (s).
    std::map<std::string, double> clock_last_seen_s_;  ///< "sat" -> last emit product time (s).
    // Constellations to force-bump at the next emit.  force_all_bump_ (set by an
    // empty-arg markDatumChange) forces every constellation, the pre-A2 global
    // semantics; forced_bump_consts_ scopes the bump to specific constellations.
    // A constellation's flag is consumed only by an emit carrying at least one of
    // its satellites (see markDatumChange).  A flagged constellation absent from an
    // emit (RT0-5 quarantine, or all its satellites set) stays pending here; a
    // consumed global bump moves its absent but tracked constellations into
    // forced_bump_consts_.
    bool           force_all_bump_{false};
    std::set<char> forced_bump_consts_;
    // Satellites to force-bump at the next emit (B1 retro-bump, markSatDiscontinuity).
    // Per-satellite scoping (phase/code/clock counters of these satellites only),
    // kept apart from the per-constellation forced_bump_consts_ so that one flagged
    // satellite does not reset a whole constellation at the consumer.  Cleared each
    // emit.
    std::set<std::string> forced_bump_sats_;
    /// Channel mask that each marked satellite was marked with (a bit-or of
    /// @ref DiscChannel), kept in step with forced_bump_sats_ (@c GnssProcessor::NetworkProductEmitter::forced_bump_sats_) and cleared with it
    ///.  Populated only under disc_mark_scope_channel_ (@c GnssProcessor::NetworkProductEmitter::disc_mark_scope_channel_), so the legacy
    /// scope allocates nothing and the emit path never reads it.
    std::map<std::string, unsigned> forced_bump_chan_;
    /// Per-emit scratch: the marks this emit withheld from a channel, and the marks
    /// it applied.  Both empty/zero under the legacy scope.
    std::vector<MarkScopeEvent> mark_scope_events_;
    MarkChannelTally            mark_channel_tally_{};
    std::map<std::string, int>    disc_counter_;    ///< "sat|signal" -> phase discontinuity counter.
    std::map<std::string, double> last_phase_bias_; ///< "sat|signal" -> last emitted phase bias (m).

    // --- Satellite clock discontinuity bookkeeping (A2; only when emit_clock_disc_) ---
    std::map<std::string, int> clock_disc_counter_; ///< "sat" -> clock discontinuity counter.
    std::set<std::string>      seen_clock_sats_;    ///< sats whose clock was emitted at least once (first-sighting marker).

    // --- Satellite code-bias discontinuity bookkeeping (H1) ---------------
    // prev_code_satset_ / prev_band_tuple_ are replaced wholesale with the current
    // epoch's maps inside emitToStore (never merged): a (const,signal) channel or a
    // satellite that disappears and later reappears is then a first sighting on a
    // live run and on a fresh replay process alike, so the counters reproduce
    // deterministically.  They update only on an actual emit (the writer persists
    // only emitted epochs, and replay must reproduce the emitted-only diff
    // sequence).
    std::map<std::string, int>    code_disc_counter_; ///< "sat|signal" -> code discontinuity counter.
    std::map<std::string, double> last_code_bias_;    ///< "sat|signal" -> last emitted code bias (m); first-sighting marker.
    std::map<std::string, std::set<std::string>> prev_code_satset_; ///< "const|signal" -> sats carrying it last emit.
    std::map<std::string, std::vector<char>>     prev_band_tuple_;  ///< "sat" -> sorted distinct code band-digits last emit (IF-pin proxy).

    // --- Optional discontinuity channels (both default off = byte-identical) --
    bool code_bias_jump_test_{false};      ///< Also bump the code counter on a fresh-baseline value jump > disc_threshold_m_.
    bool clock_disc_on_set_change_{false}; ///< Sum-to-zero datum: bump a constellation's clock counters when its clock-satellite set changes between emits.
    /// Constellation char -> sats whose clock was emitted last emit.  Replaced
    /// wholesale each emit (mirrors prev_code_satset_); populated only when
    /// clock_disc_on_set_change_ is armed, so with it off the emitter carries no
    /// extra state and the serialized emitter section is unchanged.
    std::map<char, std::set<std::string>> prev_clock_satset_;

    // --- The phase-visible step test (network.disc_phase_visible; 0 = off) ---------
    // All of this state is populated only when the test is armed, so with it off the
    // emitter carries no extra state and its serialized section is unchanged.
    int disc_pv_min_sats_{0};          ///< Minimum voters for the median; 0 = the legacy phase test.

    // --- The step-class declaration (network.disc_step_class_pairs; no family = off) -------
    // Populated only when armed, so with it off the emitter carries no extra state.
    /// One family: two phase signals on two bands, their code signals and frequencies.
    struct StepClassFamily {
        char        constellation{0};  ///< RINEX letter.
        std::string p1, p2;            ///< Phase signals of bands 1 and 2.
        std::string c1, c2;            ///< Code signals of bands 1 and 2.
        double      f1_hz{0.0};        ///< Band 1's nominal frequency (Hz).
        double      f2_hz{0.0};        ///< Band 2's nominal frequency (Hz).
    };
    /// One satellite's levels on one family at its last emission.
    struct StepClassBaseline {
        double e1{0.0};                ///< E_1 (cycles).
        double e2{0.0};                ///< E_2 (cycles).
        double epoch_s{0.0};           ///< Product time of that emission (s).
    };
    std::vector<StepClassFamily> sc_families_;              ///< The families; empty = off.
    double sc_tol_cyc_{0.15};                               ///< The class tolerance (cycles).
    std::map<std::string, StepClassBaseline> last_sc_;      ///< "<family index>|sat" -> baseline.
    bool   sc_have_prev_{false};                            ///< An armed emission preceded this one.
    double sc_prev_epoch_s_{0.0};                           ///< Its product time (s).
    StepClassCounters sc_counters_;                         ///< Run-end counters.
    long long sc_keys_stepped_{0};                          ///< Increments the class test raised.
    long long sc_value_only_{0};                            ///< Classified keys only the value test declares.
    long long sc_class_only_{0};                            ///< Classified keys only the class test declares.
    std::set<std::string> sc_release_pending_;              ///< "sat|signal" keys released, waiting for their next emission.
    long long sc_release_marks_{0};                         ///< Release marks received.
    long long sc_release_landed_{0};                        ///< Release marks landed on a key with a baseline.
    long long sc_release_only_{0};                          ///< Landed marks no other trigger stepped.
    long long sc_release_no_baseline_{0};                   ///< Release marks consumed on a first sighting.

    // --- The unsettled steps (network.ring_fix.unsettled_steps; no family = off) -------------------------------
    /// One held family: its constellation and its two phase signals.
    struct UnsettledFamily {
        char        constellation{0};  ///< RINEX letter.
        std::string p1, p2;            ///< Phase signals of bands 1 and 2.
        std::string name;              ///< "<c>:<p1>/<p2>".
    };
    std::vector<UnsettledFamily> us_families_;                         ///< The families; empty = off.
    std::map<std::string, bool> us_prev_both_;                          ///< "<family>|sat" -> both flagged at its last emission.
    std::map<std::string, std::pair<long long, long long>> us_counts_;  ///< Family -> (unsettled steps, settling steps).
    bool us_unheld_{false};                                           ///< The signals of no held family step too.
    std::map<char, std::string> us_unheld_names_;                      ///< Constellation -> "<c>:no held family".

    // --- The continuity guard (network.ring_fix.continuity_guard_cyc; threshold 0 = off) ------------------------
    /// One satellite's run under the guard.
    struct GuardRun {
        double ref{0.0};   ///< The level (narrow-lane cycles) at the run's first emission.
        int    n{0};       ///< Emissions of the run; 0 = no run.
    };
    double cg_thr_cyc_{0.0};                                         ///< Threshold; 0 = off.
    int    cg_hold_{20};                                             ///< Emissions a trip lowers the flags.
    int    cg_min_run_{4};                                           ///< Emissions before a run is compared.
    std::map<std::string, std::pair<double, double>> cg_prev_e_;     ///< "<family>|sat" -> (E_1, E_2) at the last emission.
    std::map<std::string, std::pair<double, double>> cg_datum_;      ///< Family -> running datum of E_1 and E_2.
    std::map<std::string, GuardRun> cg_run_;                         ///< "<family>|sat" -> its run.
    std::map<std::string, int> cg_hold_left_;                        ///< "sat|signal" -> emissions still lowered.
    std::map<std::string, long long> cg_trips_;                      ///< Family -> trips.
    long long cg_lowered_keys_{0};                                   ///< Phase-key emissions lowered by the guard.
    std::set<std::string> cg_lowered_last_;                          ///< "sat|signal" keys lowered at the last emission.
    std::map<std::pair<char, std::string>, std::string> pf_member_of_;   ///< Phase-family members to their identities (setPhaseFamilyMembers).
    /**
     * @brief Whether the continuity guard holds a phase key's flag lowered at this emission.
     * @param key "sat|signal".
     * @return True while a trip's hold lasts (always false when the guard is off).
     */
    bool continuityGuardLowered_(const std::string& key) const;
    /**
     * @brief The continuity guard's run test for one satellite, after its phase records are formed: continues,
     *        restarts or ends each family's run, and on a trip lowers both flags and steps both counters in @p rec.
     * @param s       The satellite's solution.
     * @param rec     Its phase-bias record of this emission (modified on a trip).
     * @param level   "<family>|sat" -> the level of this emission (narrow-lane cycles, less the family's datum).
     * @param stepped "sat|signal" keys stepped at this emission by any rule.
     */
    void continuityGuardRun_(const NetworkSatSolution& s, SsrPhaseBias& rec, const std::map<std::string, double>& level,
                             const std::set<std::string>& stepped);
    /// One key's phase-visible baseline: its clock less phase bias at its last emission
    /// with a clock, and that emission's product time.
    struct PvBaseline {
        double cmp_m{0.0};             ///< Clock less phase bias (m).
        double epoch_s{0.0};           ///< Product time of that emission (s).
    };
    std::map<std::string, PvBaseline> last_pv_;   ///< "sat|signal" -> baseline.
    bool   have_prev_emit_{false};     ///< An emission preceded this one (the test's clock of "consecutive").
    double prev_emit_epoch_s_{0.0};    ///< Product time of the previous emission (s).
    std::set<std::string> pv_prev_voters_;     ///< Keys that voted at the previous emission.
    std::set<std::string> pv_prev_below_min_;  ///< "const|signal" groups below the minimum at the previous emission.
    PhaseVisibleCounters pv_counters_{};       ///< See phaseVisibleCounters().
    PhaseVisibleEdges    pv_edges_{};          ///< See phaseVisibleEdges().
};

} // namespace GnssProcessor
