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

#include "rtcm/RtcmTypes.hpp"   // GnssTime, SSR record structs

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
    /// Hold generation that this emitted value belongs to (card PBH,
    /// @c network.emit_pb_hold).  0 means not held (the knob is off, or this key is
    /// outside the hold's scope), and the value is the estimator's own per-epoch
    /// estimate.  Nonzero means @ref bias_m was overwritten with the first value of
    /// that generation by @ref networkPbHold, which runs as the last writer before
    /// @ref NetworkProductEmitter::emitToStore, so the emitter's value-jump test and
    /// every downstream sink see the held value.  Stamped onto
    /// @c SsrPhaseBiasEntry::hold_generation and serialized as the P-record @c gen=
    /// token; a change of it is the declaration on which a consumer refloats.
    std::uint32_t hold_generation{0};
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
    /// Level step of this satellite since its previous emission (m) (card CLV,
    /// @c network.clock_level_carry).  The code-only level re-base moved its clock
    /// record and each of its phase-bias records by this amount, so the phase side
    /// that the consumer applies (-clock + phase bias) did not move.  Serialized as
    /// the C-record @c lvl= token (written only when nonzero, so an unarmed wire is
    /// byte-identical) and read back by NetworkProductReader, because the emitter's
    /// phase-bias value-jump test subtracts it from each phase-bias step before
    /// testing.  The producer and a replay use the same arithmetic, so a replay
    /// reproduces every declaration.  0 unless the card moved the level.
    double                                  level_step_m{0.0};
    /// Broadcast ephemeris version (IODE, in the IGS-SSR matching convention) that the
    /// estimable clock @c clock_c0_m is a delta against, threaded from the producer
    /// emission path in @c brdc_rts mode (RTF-2, Card 5).  -1 means sp3 mode or
    /// unstamped: @c emitToStore then leaves @c SsrClockCorrection.iode = -1 (so
    /// @c getOrbitClock's clock gate inherits the orbit anchor, byte-identical;
    /// Card 3) and the C record serializes no @c iode= field.  Card 4 owns the field,
    /// its serialization and the store stamping; Card 5 populates it.
    int                                     clock_iode{-1};
    /// IOD-SSR of the RTS orbit correction that this clock was estimated against
    /// (RTF-2, Card 5).  This is an orbit coherence echo: broadcast radial-orbit error
    /// is absorbed 1:1 into the clock, so the consumer verifies that it applied the
    /// same orbit update.  -1 means unstamped (brdc-only replay, sp3).  The orbit echo
    /// is subordinate to the @c iode= anchor: it is serialized as the C-record
    /// @c oiod= field only when @c clock_iode >= 0 (so the parent @c iode= token is
    /// written) and this field is >= 0.  A brdc_rts producer always stamps
    /// @c clock_iode (conventions §2), so "@c clock_iode >= 0" holds whenever this is
    /// set; a value set without @c clock_iode is dropped from the wire without notice.
    int                                     orbit_iod_ssr{-1};
    /// SSR content epoch of that referenced RTS orbit, the other operand of the
    /// consumer orbit/clock age-skew budget (Card 7) (RTF-2, Card 5).  {0,0} means
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
    /// @param disc_jump_test_decommon When true (card JT,
    ///        @c network.disc_jump_test_decommon), the phase-bias jump test screens
    ///        the de-commoned step, the raw epoch-to-epoch @f$\Delta PB@f$ minus the
    ///        median @f$\Delta PB@f$ over the satellites on that signal with a fresh
    ///        baseline, instead of the raw one.  A satellite-common phase-bias move is
    ///        absorbed by a consumer's own receiver phase clock, so the quantity that
    ///        reaches a consumer ambiguity is the de-commoned step (standing
    ///        invariant #5).  The raw test cannot separate a frame move from a
    ///        satellite step: in CST §4 a −1.451 m L1W frame move hid a +1.409 m
    ///        per-satellite step under a 0.0476 m bar.  The threshold is unchanged
    ///        (@ref discThresholdM).  A signal whose fresh-baseline population is
    ///        below @ref kDiscDecommonMinSats falls back to the raw test (counted).
    ///        Default false: byte-identical.  Travels in the S contract
    ///        (@c discdecom=) for @c makeReplayEmitter.
    /// @param disc_mark_scope_channel When true (card MS, @c network.disc_mark_scope),
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
    explicit NetworkProductEmitter(double disc_threshold_m = kDefaultDiscThresholdM,
                                   bool emit_clock_disc = false,
                                   double staleness_horizon_s = 0.0,
                                   bool code_bias_jump_test = false,
                                   bool clock_disc_on_set_change = false,
                                   double disc_threshold_frac_cycles = 0.0,
                                   bool disc_jump_test_decommon = false,
                                   bool disc_mark_scope_channel = false);

    /// Product channels that a per-satellite discontinuity mark applies to (card MS).
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
    /// excluded it: invariant #7's identity for the decoupling edge (card MS).
    struct MarkScopeEvent {
        std::string sat;              ///< RINEX satellite id.
        unsigned    marked{0};        ///< The channel mask the marking sites asked for.
        DiscChannel suppressed{kDiscChanClock};  ///< The channel that was not bumped.
    };

    /// Suppressed marks of the last @ref emitToStore call, cleared at the top of every
    /// emit (card MS).  Always empty under the legacy satellite scope, so a run with
    /// the knob off allocates nothing and the driver's drain is a no-op.
    const std::vector<MarkScopeEvent>& markScopeEvents() const { return mark_scope_events_; }

    /// Number of marks this emit applied to each channel, indexed by
    /// @ref DiscChannel (clock / code-bias / phase-bias) (card MS).  Cleared per emit;
    /// always zero under the legacy satellite scope (nothing is counted there).
    struct MarkChannelTally {
        long long clk{0};   ///< Satellite-epochs whose clock generation a mark bumped.
        long long cb{0};    ///< (sat, signal) code-bias cells a mark bumped.
        long long pb{0};    ///< (sat, signal) phase-bias cells a mark bumped.
    };
    /// Applied-mark tally of the last @ref emitToStore call (card MS).
    const MarkChannelTally& markChannelTally() const { return mark_channel_tally_; }

    /// Whether the per-channel mark scope is armed on this emitter (card MS).
    bool discMarkScopeChannel() const { return disc_mark_scope_channel_; }

    /**
     * @brief Channel mask of the marks pending for one satellite (card PBH).
     *
     * A pure read of the same two sources that @c emitToStore's internal
     * @c constForced / @c satForced lambdas consult, without their side effects
     * (no tally, no @ref MarkScopeEvent row).  A caller can therefore ask what is
     * about to be declared for this satellite before the emit without changing the
     * card-MS census by a single count.
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

    /// Stable lowercase name of one channel ("clk" / "cb" / "pb"), for trace events
    /// and tests (card MS).
    static const char* discChannelName(DiscChannel c);

    /// Minimum number of fresh-baseline satellites that a signal must carry before its
    /// de-commoned median is trusted as a frame estimate (card JT).  Below it the
    /// (sat, signal) cells of that signal fall back to the raw jump test and are
    /// counted as @c fallback_raw, because the median of a thin signal does not
    /// estimate a frame.
    static constexpr int kDiscDecommonMinSats = 3;

    /// How the de-commoned and the raw phase jump tests disagreed on one
    /// (sat, signal) cell: invariant #7's identity for both edges (card JT).
    enum class PbJumpDecommonClass {
        DecommonOnly,       ///< De-commoned fires, raw would not: a frame move hid the step, which the raw test misses.
        Both,               ///< Both fire: a step large enough to clear the bar in either frame.
        RawOnlySuppressed,  ///< Raw fires, de-commoned does not: the common-mode class (the frame moved, this satellite did not).
        FallbackRaw         ///< The signal's population was below @ref kDiscDecommonMinSats; the raw result stands.
    };

    /// One classified cell of the epoch's phase jump test (card JT).  Emitted only for
    /// cells that the jump test decided: a cell already forced by
    /// @ref markDatumChange / @ref markSatDiscontinuity is declared regardless and is
    /// not classified, so the census counts only the declarations this knob decides.
    struct PbJumpDecommonEvent {
        std::string         sat;               ///< RINEX satellite id.
        std::string         signal;            ///< RINEX obs code of the phase signal.
        double              raw_delta_m{0.0};  ///< Raw ΔPB against this key's baseline (m, signed).
        double              common_m{0.0};     ///< The signal's median ΔPB (m); 0 on a @c FallbackRaw cell.
        double              decommon_m{0.0};   ///< raw_delta_m − common_m (m); equals raw on a @c FallbackRaw cell.
        double              threshold_m{0.0};  ///< The bar this cell was screened against (@ref discThresholdM).
        int                 population{0};     ///< Satellites in the signal's median population.
        bool                declared{false};   ///< Whether the armed test declared (stepped the counter).
        PbJumpDecommonClass cls{PbJumpDecommonClass::Both};  ///< Which class this cell fell in.
    };

    /// @brief Number of phase-bias value-jump tests whose raw step exceeded the bar
    ///        only because of the satellite's declared level step
    ///        (@ref NetworkSatSolution::level_step_m, the C-record lvl= token), and
    ///        which therefore did not declare once the step was removed (card CLV).
    /// @return the run count (0 on an unarmed run).
    long long clvJumpExcluded() const { return clv_jump_excluded_; }

    /// Classified cells of the last @ref emitToStore call, cleared at the top of every
    /// emit (card JT).  Always empty when the knob is off, so a run with the knob off
    /// allocates nothing and the driver's drain is a no-op.
    const std::vector<PbJumpDecommonEvent>& pbJumpDecommonEvents() const {
        return pb_jump_decommon_events_;
    }

    /// Whether the de-commoned phase jump test is armed on this emitter (card JT).
    bool discJumpTestDecommon() const { return disc_jump_test_decommon_; }

    /// Serialize @p sol into @p store (Sink A: in-process, no wire bytes).
    void emitToStore(const NetworkSolutionEpoch& sol, SsrCorrectionStore& store);

    /// Force a discontinuity bump at the next emit, scoped to the given constellation
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
    void markDatumChange(const std::set<char>& constellations = {});

    /// Force a discontinuity bump at the next emit, scoped to the given satellite ids
    /// (e.g. {"G13","E09"}).  This is the B1 retro-bump: a post-emit QC tier flagged
    /// these satellites' corrections as contaminated, so the consumer must reset the
    /// ambiguity that integrated the one bad epoch.  Unlike @c markDatumChange (a
    /// whole-constellation datum re-reference) this steps only the named satellites'
    /// phase, code and clock counters, a per-satellite reset rather than a reset of
    /// every satellite in the constellation (P1-3).  Applies to the next emit only.
    ///
    /// @param satellites The satellites to mark.
    /// @param channels   Which product channels this mark reports, a bit-or of
    ///        @ref DiscChannel (card MS).  Honoured only when the emitter was
    ///        constructed with @c disc_mark_scope_channel; under the legacy satellite
    ///        scope every mark bumps all three generations, so passing a narrower mask
    ///        is byte-identical there.  The default @ref kDiscChanAll is what an
    ///        un-migrated caller, and a product replay (whose @c R record carries no
    ///        channel set), asks for.  Two sites marking the same satellite in one
    ///        epoch OR their masks.
    void markSatDiscontinuity(const std::set<std::string>& satellites,
                              unsigned channels = kDiscChanAll);

    /// One constellation's tally in a @ref pbSeamCensus: how many of its phase-bias
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
     * Read-only twin of the jump test inside @ref emitToStore: same population (a key
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

    /// Current phase-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    int discontinuityCounter(const std::string& sat, const std::string& signal) const;

    /// Current code-bias discontinuity counter for a (sat, signal), or 0 if untracked.
    int codeBiasDiscontinuityCounter(const std::string& sat, const std::string& signal) const;

    /// Current clock discontinuity counter for a satellite, or 0 if untracked.
    /// Always 0 unless the emitter was constructed with @c emit_clock_disc (A2).
    int clockDiscontinuityCounter(const std::string& sat) const;

    /**
     * @brief Whether this phase-bias key is a first sighting (card SID phase 5, E1).
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

    /// Code-bias twin of @ref isPhaseFirstSighting (card SID phase 5, E1).
    bool isCodeFirstSighting(const std::string& sat, const std::string& signal) const;

    /// Clock twin of @ref isPhaseFirstSighting (card SID phase 5, E1).  Always true
    /// unless the emitter was constructed with @c emit_clock_disc (A2), which is what
    /// fills @c seen_clock_sats_.
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
     * @c forced_bump_sats_, with each mark's channel mask under the card-MS channel
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

private:
    double disc_threshold_m_;
    /// When > 0, the per-(sat,signal) jump threshold is disc_threshold_frac_cycles_ ×
    /// λ(sat,signal) (fixed contract, uniform in cycles).  0 = legacy flat metres (H3).
    double disc_threshold_frac_cycles_{0.0};
    /// The jump-test threshold (m) for a given (sat, signal): frac·λ when the
    /// fractional-cycle mode is armed and λ resolves, else the flat metre gate.
    double discThresholdM(const std::string& sat, const std::string& signal) const;
    /// Screen the de-commoned phase step (raw minus the signal's median) rather than
    /// the raw one (card JT).  Default false = the legacy raw test, which does not
    /// separate frame moves from satellite steps (byte-identical).  Travels in the
    /// S contract (@c discdecom=).
    bool   disc_jump_test_decommon_{false};
    /// Bump only the channels that a mark named (card MS).  Default false = the
    /// legacy satellite-wide scope (byte-identical).  Travels in the S contract
    /// (@c discscope=).  Declared here so that the ctor's init list stays in
    /// declaration order.
    bool   disc_mark_scope_channel_{false};
    /// Classified cells of the last emit, empty unless armed (card JT).  Per-emit
    /// scratch, not serialized: the de-commoned step is a pure function of this
    /// epoch's emitted biases and @ref last_phase_bias_, so a replay reproduces it
    /// with no extra stream state.
    std::vector<PbJumpDecommonEvent> pb_jump_decommon_events_;
    long long clv_jump_excluded_{0};   ///< See clvJumpExcluded() (card CLV).
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
    /// @ref DiscChannel), kept in step with @ref forced_bump_sats_ and cleared with it
    /// (card MS).  Populated only under @ref disc_mark_scope_channel_, so the legacy
    /// scope allocates nothing and the emit path never reads it.
    std::map<std::string, unsigned> forced_bump_chan_;
    /// Per-emit scratch: the marks this emit withheld from a channel, and the marks
    /// it applied (card MS).  Both empty/zero under the legacy scope.
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
};

} // namespace GnssProcessor
