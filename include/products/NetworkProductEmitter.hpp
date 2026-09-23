#pragma once
/**
 * @file NetworkProductEmitter.hpp
 * @brief Producer-side serializer for the PPP-RTK NETWORK joint solution.
 *
 * Sink A of the network product-emit path (W8): maps one epoch of the network's
 * estimated satellite-side solution (estimable satellite clock + code/phase
 * bias) into the existing SSR record structs and pushes them into a
 * @c SsrCorrectionStore via its push API — in the exact IGS-SSR convention the
 * USER consumer reads.  This makes the existing, battle-tested user pipeline the
 * closed-loop integer-recovery validation harness: "does our own user engine
 * recover integers from our own products on a held-out station?"
 *
 * The emitter does NOT estimate; it serializes a @c NetworkSolutionEpoch the
 * network estimator (W4) produces.  It DOES own the discontinuity bookkeeping —
 * stream state the estimator cannot know: it tracks the last emitted phase bias
 * per (sat, signal) and increments the @c discontinuity_counter on a bias jump
 * (or, via @c markDatumChange, on a global pivot / S-basis re-declaration, which
 * W6b wires in).  The consumer's @c noteSsrDiscontinuity -> @c resetAmbiguity
 * hook turns that increment into a clean ambiguity reset.
 *
 * It owns the analogous bookkeeping for satellite CODE biases (H1): the
 * per-(constellation, signal) sum-to-zero datum re-references whenever the
 * satellite SET carrying a signal changes (rise/set), and the per-satellite
 * ionosphere-free pin re-references whenever that satellite's distinct-band set
 * changes — both step the emitted code biases.  The emitter detects both from
 * the epoch's own @c code_bias_m composition (so replay reproduces the counters
 * with NO new serialization, exactly as for phase), and also honours the same
 * @c markDatumChange forced bump (a clock-pivot handover re-references the whole
 * shared solution).  By default there is no magnitude threshold for code: the
 * deterministic set/band-change detectors cover every GAUGE re-reference, and a
 * code RW noise step is not a datum discontinuity.  The Jul-2026 wire audit,
 * however, measured large unsignalled code-bias VALUE jumps beyond what the
 * structural triggers catch (weak-separation wander at thin rosters; 7.6k jumps
 * vs 2.8k signalled at net305), so an OPT-IN magnitude jump test
 * (@c network.code_bias_disc_jump_test, same threshold as phase) can be layered
 * on top — honest signalling for whatever still moves.  The consumer's
 * @c getCodeBiasDiscontinuity -> @c reinflateCodeBiasVariance hook turns an
 * increment into a clean receiver code-bias re-inflation.
 *
 * The CLOCK channel (A2) bumps only on forced datum changes (pivot handover /
 * retro-bump) by default.  Under the SUM-TO-ZERO clock datum the gauge instead
 * re-references whenever a constellation's clock-satellite SET changes (the
 * survivors redistribute ∓ΔC0/N) — an OPT-IN set-change trigger
 * (@c network.clock_disc_on_set_change) detects that from the epoch's own clock
 * composition (replay-reproducible, like the code set trigger) and steps the
 * constellation's clock counters so the datum move is signalled on the wire.
 *
 * Sink B (RTCM-4076 wire encoding) is deferred to Phase 2.
 *
 * NOTE: push into an OWNED (non-singleton) SsrCorrectionStore for the closed
 * loop — a producer must never share the store it (or its consumer) reads from
 * the global singleton, and tests must not pollute global state.
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
    double sigma_m{0.0};          ///< B1b: formal 1σ (m) of this phase-bias correction; 0 unless the driver populated it (network.emit_phase_bias_sigma, gauge-exact shared-cov diagonal).
    double sigc_m{0.0};           ///< M4 (contract v2): formal 1σ (m) of the COMBINED PHASE correction (clock + phase bias, cross-cov included; NO iono); 0 unless network.emit_combined_sigma populated it. Serialized as the P-record sigc= token; consumed via SsrPhaseBiasEntry::combined_sigma_m.
    /// Card PBH (@c network.emit_pb_hold): the HOLD GENERATION this emitted value
    /// belongs to.  0 = not held (the knob is off, or this key is outside the hold's
    /// scope) and the value is the estimator's own per-epoch estimate, exactly as
    /// before.  Nonzero = @ref bias_m was overwritten with the first value of that
    /// generation by @ref networkPbHold, which runs as the LAST writer before
    /// @ref NetworkProductEmitter::emitToStore — so the emitter's value-jump test and
    /// every downstream sink see the held value.  Stamped onto
    /// @c SsrPhaseBiasEntry::hold_generation and serialized as the P-record @c gen=
    /// token; a CHANGE of it is the declaration a consumer re-floats on.
    std::uint32_t hold_generation{0};
};

/// One satellite's estimated satellite-side solution for a single epoch.
struct NetworkSatSolution {
    std::string                             sat;            ///< RINEX sat ID ("G01", "E11").
    bool                                    has_clock{false};  ///< Whether @c clock_c0_m is valid.
    double                                  clock_c0_m{0.0}; ///< Estimable satellite clock C0 (m).
    double                                  clock_c1_m_s{0.0}; ///< Estimable satellite clock drift C1 (m/s); 0 unless network.clock_model on (B3b — the two-state oscillator drift the consumer extrapolates as c0+c1·dt).
    double                                  clock_ura_m{0.0}; ///< B1: per-sat URA 1σ (m) = EWMA(σ̂0)×formal clock σ; 0 unless has_ura.
    bool                                    has_ura{false};   ///< B1: whether clock_ura_m is populated (network.emit_ura on).
    /// M4 (contract v2): formal 1σ (m) of the COMBINED CODE correction (clock +
    /// code bias at a reference signal, cross-cov included); 0 unless
    /// network.emit_combined_sigma populated it.  Per-sat (the C record).
    /// Serialized as the C-record sigcc= token; store home
    /// SsrClockCorrection::combined_code_sigma_m.
    double                                  sigcc_m{0.0};
    /// M4 (contract v2): correlation ρ between the combined CODE and combined PHASE
    /// corrections at the reference signal (both share the clock term ⇒ ~0.6–0.9);
    /// 0 unless populated.  Serialized as the C-record rho= token.
    double                                  rho{0.0};
    /// Card CLV (@c network.clock_level_carry): the LEVEL STEP of this satellite since
    /// its previous emission (m): the code-only level re-base moved its clock record
    /// AND every one of its phase-bias records by this amount, so the phase side the
    /// consumer applies (-clock + phase bias) did not move.  Serialized as the C-record
    /// @c lvl= token (written only when nonzero, so an unarmed wire is byte-identical)
    /// and read back by NetworkProductReader, because the emitter's phase-bias
    /// value-jump test removes it from each phase-bias step before testing (the same
    /// arithmetic in the producer and in a replay, so a replay reproduces every
    /// declaration).  0 unless the card moved the level.
    double                                  level_step_m{0.0};
    /// RTF-2 (Card 5): broadcast ephemeris version (IODE, in the IGS-SSR matching
    /// convention) the estimable clock @c clock_c0_m is a delta against, threaded
    /// from the producer emission path in @c brdc_rts mode.  -1 = sp3 mode /
    /// unstamped: @c emitToStore then leaves @c SsrClockCorrection.iode = -1 (so
    /// @c getOrbitClock's clock gate inherits the orbit anchor, byte-identical —
    /// Card 3) and the C record serializes NO @c iode= field.  Card 4 owns the
    /// field + its serialization + store stamping; Card 5 populates it.
    int                                     clock_iode{-1};
    /// RTF-2 (Card 5): IOD-SSR of the RTS orbit correction this clock was estimated
    /// against (orbit coherence echo — broadcast radial-orbit error absorbs 1:1
    /// into the clock, so the consumer verifies it applied the SAME orbit update).
    /// -1 = unstamped (brdc-only replay / sp3).  The orbit echo is SUBORDINATE to
    /// the @c iode= anchor: serialized as the C-record @c oiod= field only when
    /// @c clock_iode >= 0 (the parent @c iode= token is written) AND this field is
    /// >= 0.  A brdc_rts producer always stamps @c clock_iode (conventions §2), so
    /// "@c clock_iode >= 0" holds whenever this is set — but set this WITHOUT
    /// @c clock_iode and it is silently dropped from the wire.
    int                                     orbit_iod_ssr{-1};
    /// RTF-2 (Card 5): SSR content epoch of that referenced RTS orbit — the other
    /// operand of the consumer orbit/clock age-skew budget (Card 7).  {0,0} =
    /// unstamped.  Like @c orbit_iod_ssr it is subordinate to the @c iode= anchor:
    /// serialized as the C-record @c oepoch= field only when @c clock_iode >= 0 AND
    /// week != 0.
    GnssTime                                orbit_epoch{0, 0.0};
    std::map<std::string, double>           code_bias_m;    ///< RINEX obs code -> satellite code bias (m).
    /// RF-0f (@c network.sat_isc_state): the subset of @ref code_bias_m whose signal is
    /// a same-band companion on the T_GD pin's SECOND band and whose inter-signal
    /// correction was ESTIMATED — some station tracked both that signal and the pinned
    /// one on this satellite, so `ISC^s = b(companion) - b(pinned)` is determined by
    /// data.  Those records carry `isc=obs` on the wire.  A companion whose ISC had to
    /// be pinned at 0 is an ASSERTION a consumer cannot tell from an estimate by value,
    /// so it is WITHHELD from @ref code_bias_m entirely rather than flagged.  Empty
    /// unless the knob is armed ⇒ every record is byte-identical.
    std::set<std::string>                   isc_observed;
    std::map<std::string, NetworkPhaseBias> phase_bias;     ///< RINEX obs code -> satellite phase bias.
};

/// One epoch of the network joint solution to serialize.
struct NetworkSolutionEpoch {
    GnssTime                        epoch_time{0, 0.0}; ///< SSR content reference epoch.
    /// RT0-4: wall-clock TIME-OF-GENERATION (when the producer solved this epoch),
    /// distinct from @c epoch_time (the content reference).  Left {0,0} unless the
    /// producer stamps it (network.emit_generation_time on) — a {0,0} value means
    /// "not stamped" and serializes NO G record, so the product is byte-identical.
    /// A consumer measures product latency as (ingest wall-clock − generation_time).
    GnssTime                        generation_time{0, 0.0};
    /// RTA-3: solution-GENERATION id — a per-run marker (uint32 derived from the
    /// FIRST EMITTED epoch's GPS time, week·604800 + ⌊tow⌋) distinguishing one
    /// producer datum realization from another across restarts.  0 means "not
    /// stamped" (network.emit_generation_id off): no @c I record is serialized and
    /// the store's generation field stays unset, so the product is byte-identical.
    /// Unlike @c iod_ssr (a per-epoch increment, meaningless as a generation
    /// marker) this is CONSTANT for a run and deterministic under file replay (it
    /// is serialized per epoch), while a restarted live producer — which first-
    /// emits at a later epoch — necessarily mints a different id.  A consumer
    /// treats a CHANGE of a present id as a global datum break.
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
    /// Default phase-bias jump (m) that counts as a discontinuity (R9: single source of
    /// truth for the literal that the driver's emitter construction and the serialized
    /// S-basis contract's @c disc_threshold_m must agree on).  0.05 m is well below a GPS
    /// L1 cycle (~0.19 m) so genuine cycle-level bias jumps trip it while noise does not.
    static constexpr double kDefaultDiscThresholdM = 0.05;

    /// @param disc_threshold_m Phase-bias jump (m) that counts as a discontinuity.
    ///        Defaults to @ref kDefaultDiscThresholdM.
    /// @param emit_clock_disc When true (A2), the emitter maintains a per-satellite
    ///        CLOCK discontinuity counter (bumped on a scoped datum change for that
    ///        satellite's constellation) and stamps it on every pushed
    ///        @c SsrClockCorrection.  Default false leaves @c disc_counter at 0 —
    ///        byte-identical to the pre-A2 store.
    /// @param staleness_horizon_s RTA-11: staleness horizon in seconds of PRODUCT
    ///        time (never wall clock — replay must be deterministic).  When > 0 the
    ///        emitter tracks the last-emitted epoch time per (sat[,signal]) key; a
    ///        key reappearing after a gap larger than this is treated as a FIRST
    ///        SIGHTING on the phase-bias, code-bias and clock channels alike (the
    ///        baseline reseeds; no jump-test or forced bump lands) — a return after
    ///        a multi-hour vanish is a first-sighting-after-gap, not a fault.  The
    ///        counter VALUE persists (it never resets), so a consumer that somehow
    ///        kept state sees no change.  Default 0 keeps the legacy keep-forever
    ///        baselines (byte-identical).  Like the other two parameters it travels
    ///        in the serialized S contract (@c stale=) for @c makeReplayEmitter.
    /// @param code_bias_jump_test When true (@c network.code_bias_disc_jump_test),
    ///        the CODE-bias counter also bumps on a fresh-baseline VALUE jump
    ///        beyond @p disc_threshold_m — the phase jump test applied to code, on
    ///        top of the always-on structural set/band-change triggers.  Signals
    ///        the weak-separation value wander the structural triggers cannot see.
    ///        Default false = structural triggers only (byte-identical).  Travels
    ///        in the S contract (@c cbjump=) for @c makeReplayEmitter.
    /// @param clock_disc_on_set_change When true
    ///        (@c network.clock_disc_on_set_change; requires the sum-to-zero clock
    ///        datum + @p emit_clock_disc), a change of a constellation's
    ///        clock-satellite SET between consecutive emits steps that
    ///        constellation's clock counters — under Σδtˢ=0 a membership change IS
    ///        a datum re-reference (the survivors redistribute), previously
    ///        unsignalled.  Detected from the emitted epoch itself
    ///        (replay-reproducible).  Default false (byte-identical).  Travels in
    ///        the S contract (@c clkset=) for @c makeReplayEmitter.
    /// @param disc_threshold_frac_cycles When > 0 (@c network.disc_threshold_frac_cycles),
    ///        the phase (and, under @p code_bias_jump_test, code) jump test uses a
    ///        PER-(sat,signal) threshold of @p disc_threshold_frac_cycles × λ(sat,signal)
    ///        instead of the flat @p disc_threshold_m — a still-FIXED contract (λ is a
    ///        public, static band identity, deterministic on producer and replay
    ///        consumer alike) that makes the gate uniform in CYCLES rather than metres.
    ///        The recommended 0.25 gives L1/E1 4.76 cm ≈ the legacy 0.05 m, L2 6.1 cm,
    ///        L5/E5a 6.37 cm.  Default 0.0 = the legacy flat @p disc_threshold_m
    ///        (byte-identical).  Travels in the S contract (@c discfrac=) for
    ///        @c makeReplayEmitter.
    /// @param disc_jump_test_decommon Card JT (@c network.disc_jump_test_decommon):
    ///        when true the PHASE-bias jump test screens the DE-COMMONED step —
    ///        the raw epoch-to-epoch @f$\Delta PB@f$ minus the median @f$\Delta PB@f$
    ///        over the satellites emitted on that SIGNAL at both epochs — instead of
    ///        the raw one.  A satellite-common phase-bias move is absorbed by a
    ///        consumer's own receiver phase clock, so the quantity that actually
    ///        lands in a consumer ambiguity is the de-commoned step (standing
    ///        invariant #5); the raw test is frame-blind and CST §4 measured a
    ///        −1.451 m L1W frame move hiding a +1.409 m per-satellite step under a
    ///        0.0476 m bar.  The threshold is unchanged (@ref discThresholdM).  A
    ///        signal whose fresh-baseline population is below
    ///        @ref kDiscDecommonMinSats falls back to the RAW test (counted).
    ///        Default false ⇒ byte-identical.  Travels in the S contract
    ///        (@c discdecom=) for @c makeReplayEmitter.
    /// @param disc_mark_scope_channel Card MS (@c network.disc_mark_scope):
    ///        when true a per-satellite discontinuity mark bumps ONLY the
    ///        generations of the CHANNELS the marking site says moved
    ///        (@ref markSatDiscontinuity's @c channels argument), instead of the
    ///        clock, code-bias AND phase-bias generation of that satellite alike.
    ///        A clock-only guard (the Q3 anchor decouple, the CLK-R step test)
    ///        therefore stops declaring a phase-bias step that did not happen —
    ///        measured on RF-0hv: 117 of 390 declared consumer cell-epochs were
    ///        EMPTY (< 0.02 m of PB motion, CST §7.1).  The emitter's own
    ///        per-(sat, signal) jump tests are untouched and remain the backstop
    ///        for a genuine re-realization on a channel a mark did not name.
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

    /// Card MS: the product CHANNELS a per-satellite discontinuity mark applies
    /// to.  A bit mask so a marking site that genuinely moves two channels names
    /// both, and two sites marking the same satellite in one epoch OR together.
    enum DiscChannel : unsigned {
        kDiscChanClock     = 1u << 0,  ///< The satellite CLOCK generation (C record).
        kDiscChanCodeBias  = 1u << 1,  ///< The satellite CODE-BIAS generations (B records).
        kDiscChanPhaseBias = 1u << 2,  ///< The satellite PHASE-BIAS generations (P records).
        /// Every channel — the legacy satellite-wide scope, and what an
        /// un-migrated caller (including a product REPLAY, whose @c R record
        /// carries no channel set) asks for by omitting the argument.
        kDiscChanAll = kDiscChanClock | kDiscChanCodeBias | kDiscChanPhaseBias
    };

    /// Card MS: one mark this emit did NOT apply to a channel because the mark's
    /// scope excluded it — invariant #7's identity for the DECOUPLING edge.
    struct MarkScopeEvent {
        std::string sat;              ///< RINEX satellite id.
        unsigned    marked{0};        ///< The channel mask the marking sites asked for.
        DiscChannel suppressed{kDiscChanClock};  ///< The channel that was NOT bumped.
    };

    /// Card MS: the suppressed marks of the LAST @ref emitToStore call (cleared at
    /// the top of every emit).  Always EMPTY under the legacy satellite scope, so an
    /// OFF run allocates nothing and the driver's drain is a no-op.
    const std::vector<MarkScopeEvent>& markScopeEvents() const { return mark_scope_events_; }

    /// Card MS: how many marks this emit APPLIED to each channel — index with
    /// @ref DiscChannel (clock / code-bias / phase-bias).  Cleared per emit; always
    /// zero under the legacy satellite scope (nothing is counted there).
    struct MarkChannelTally {
        long long clk{0};   ///< Satellite-epochs whose CLOCK generation a mark bumped.
        long long cb{0};    ///< (sat, signal) code-bias cells a mark bumped.
        long long pb{0};    ///< (sat, signal) phase-bias cells a mark bumped.
    };
    /// Card MS: the applied-mark tally of the LAST @ref emitToStore call.
    const MarkChannelTally& markChannelTally() const { return mark_channel_tally_; }

    /// Card MS: whether the per-channel mark scope is armed on this emitter.
    bool discMarkScopeChannel() const { return disc_mark_scope_channel_; }

    /**
     * @brief Card PBH: the channel mask of the marks PENDING for one satellite.
     *
     * A pure read of the same two sources @c emitToStore's internal
     * @c constForced / @c satForced lambdas consult, with none of their
     * side effects (no tally, no @ref MarkScopeEvent row) — so a caller can ask
     * "what is about to be declared for this satellite" before the emit without
     * perturbing the card-MS census by a single count.
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

    /// Card MS: stable lowercase name of one channel ("clk" / "cb" / "pb"), for
    /// trace events and tests.
    static const char* discChannelName(DiscChannel c);

    /// Card JT: minimum number of fresh-baseline satellites a SIGNAL must carry
    /// before its de-commoned median is trusted as a frame estimate.  Below it the
    /// (sat, signal) cells of that signal fall back to the RAW jump test and are
    /// counted as @c fallback_raw — a thin signal's "median" is not a frame.
    static constexpr int kDiscDecommonMinSats = 3;

    /// Card JT: which way the de-commoned and the raw phase jump tests disagreed on
    /// one (sat, signal) cell — invariant #7's identity for both edges.
    enum class PbJumpDecommonClass {
        DecommonOnly,       ///< De-commoned fires, raw would NOT: the previously MISSED class (a frame move hid the step).
        Both,               ///< Both fire — a step large enough to clear the bar in either frame.
        RawOnlySuppressed,  ///< Raw fires, de-commoned does NOT: the COMMON-MODE class (the frame moved, this satellite did not).
        FallbackRaw         ///< The signal's population was below @ref kDiscDecommonMinSats; the RAW result stands.
    };

    /// Card JT: one classified cell of the epoch's phase jump test.  Emitted only for
    /// cells the jump test actually decided (a cell already forced by
    /// @ref markDatumChange / @ref markSatDiscontinuity is declared regardless and is
    /// NOT classified — the census counts declarations this knob is responsible for).
    struct PbJumpDecommonEvent {
        std::string         sat;               ///< RINEX satellite id.
        std::string         signal;            ///< RINEX obs code of the phase signal.
        double              raw_delta_m{0.0};  ///< Raw ΔPB against this key's baseline (m, signed).
        double              common_m{0.0};     ///< The signal's median ΔPB (m) — 0 on a @c FallbackRaw cell.
        double              decommon_m{0.0};   ///< raw_delta_m − common_m (m) — equals raw on a @c FallbackRaw cell.
        double              threshold_m{0.0};  ///< The bar this cell was screened against (@ref discThresholdM).
        int                 population{0};     ///< Satellites in the signal's median population.
        bool                declared{false};   ///< Whether the armed test declared (stepped the counter).
        PbJumpDecommonClass cls{PbJumpDecommonClass::Both};  ///< Which class this cell fell in.
    };

    /// @brief Card CLV: phase-bias value-jump tests whose raw step exceeded the bar
    ///        only because of the satellite's declared level step
    ///        (@ref NetworkSatSolution::level_step_m, the C-record lvl= token), and
    ///        which the step's removal therefore did not declare.
    /// @return the run count (0 on an unarmed run).
    long long clvJumpExcluded() const { return clv_jump_excluded_; }

    /// Card JT: the classified cells of the LAST @ref emitToStore call (cleared at the
    /// top of every emit).  Always EMPTY when the knob is off, so an OFF run allocates
    /// nothing and the driver's drain is a no-op.
    const std::vector<PbJumpDecommonEvent>& pbJumpDecommonEvents() const {
        return pb_jump_decommon_events_;
    }

    /// Card JT: whether the de-commoned phase jump test is armed on this emitter.
    bool discJumpTestDecommon() const { return disc_jump_test_decommon_; }

    /// Serialize @p sol into @p store (Sink A — in-process, no wire bytes).
    void emitToStore(const NetworkSolutionEpoch& sol, SsrCorrectionStore& store);

    /// Force a discontinuity bump at the NEXT emit, scoped to the given
    /// constellation chars (e.g. {'G','E'}) — a per-constellation pivot handover /
    /// S-basis re-declaration.  An EMPTY set forces EVERY constellation (the
    /// backward-compatible global bump, matching the pre-A2 single-flag behavior).
    ///
    /// Pending semantics (RT0-5 seam): a constellation's flag is CONSUMED only by
    /// an emit that carries at least one of its satellites.  If the next emitted
    /// epoch has zero satellites of a flagged constellation (e.g. the RT0-5
    /// clock-runaway quarantine stripped them), the flag stays pending and the
    /// bump lands at the first emit where that constellation reappears — the
    /// discontinuity signal describes the last EMITTED product and cannot be lost
    /// to a quarantine/datum-change coincidence.  Repeated marks while pending
    /// coalesce into a single bump.  Deterministic on replay: the pending state is
    /// a pure function of the emitted epoch sequence plus the markDatumChange
    /// calls, both of which the product file reproduces (E-line datum_consts on
    /// the trigger epoch + the same pruned epochs).
    void markDatumChange(const std::set<char>& constellations = {});

    /// Force a discontinuity bump at the NEXT emit, scoped to the given SATELLITE
    /// ids (e.g. {"G13","E09"}) — the B1 retro-bump: a post-emit QC tier flagged
    /// these satellites' corrections as contaminated, so the consumer must reset the
    /// ambiguity that integrated the one bad epoch.  Unlike @c markDatumChange (a
    /// whole-constellation datum re-reference) this steps ONLY the named satellites'
    /// phase, code, and clock counters — a per-satellite reset, not a
    /// constellation-wide reset storm (P1-3).  Applies to exactly the next emit.
    ///
    /// @param satellites The satellites to mark.
    /// @param channels   Card MS: which product channels this mark reports
    ///        (a bit-or of @ref DiscChannel).  Honoured ONLY when the emitter was
    ///        constructed with @c disc_mark_scope_channel; under the legacy
    ///        satellite scope every mark bumps all three generations exactly as
    ///        before, so passing a narrower mask is byte-identical there.  The
    ///        default @ref kDiscChanAll is what an un-migrated caller — and a
    ///        product REPLAY, whose @c R record carries no channel set — asks for.
    ///        Two sites marking the same satellite in one epoch OR their masks.
    void markSatDiscontinuity(const std::set<std::string>& satellites,
                              unsigned channels = kDiscChanAll);

    /// RS-4 (SPEC_event_harness §21.9): one constellation's tally in a
    /// @ref pbSeamCensus — how many of its phase-bias keys the jump test WOULD
    /// look at this epoch, and how many of those exceed @c discThresholdM.
    struct PbSeamConstTally {
        long long keys_tested{0};   ///< Keys carrying a non-stale baseline (the jump test's own population).
        long long keys_over{0};     ///< Of those, keys whose |bias − baseline| exceeds discThresholdM.
        double    max_abs_delta_m{0.0}; ///< Largest |bias − baseline| over the tested keys (m) — the event's statistic.
    };

    /**
     * @brief RS-4: census of this epoch's emitted phase biases against the CURRENT
     *        per-key baselines, WITHOUT mutating any emitter state.
     *
     * Read-only twin of the jump test inside @ref emitToStore: same population (a key
     * with a baseline that is not stale under the staleness horizon), same threshold
     * (@c discThresholdM), same key spelling. Call it BEFORE @ref emitToStore — after
     * it, every baseline has been overwritten with this epoch's value and the census
     * is trivially empty.
     *
     * Its purpose is the warm-resume seam: on a restored emitter the baselines are the
     * PREVIOUS run's emitted values, so a level re-realization at the first post-resume
     * emit shows up here as a large tested-key fraction — the thing
     * @c network.resume_pb_seam_mark turns into a @c markDatumChange. Const and
     * side-effect free, so an OFF run that never calls it is byte-identical.
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
     * @brief Card SID phase 5 (E1): is this PHASE-BIAS key a FIRST SIGHTING?
     *
     * The emitter's own test: @c last_phase_bias_.find(key) == end(): exposed
     * so it can be compared with the arc ledger, which knows whether the family
     * is returning.  A first sighting emits no discontinuity and re-seeds the
     * baseline, so a RETURN read as a first sighting is a re-coupling edge the
     * consumer never hears about (standing invariant 7).
     *
     * ⚠ READ BEFORE @ref emitToStore.  That call overwrites the baselines with
     * this epoch's values, after which every key answers false.
     *
     * @param sat    Satellite id.
     * @param signal RINEX signal.
     * @return true when the emitter has no baseline for this key.
     */
    bool isPhaseFirstSighting(const std::string& sat, const std::string& signal) const;

    /// Card SID phase 5 (E1): the CODE-BIAS twin of @ref isPhaseFirstSighting.
    bool isCodeFirstSighting(const std::string& sat, const std::string& signal) const;

    /// Card SID phase 5 (E1): the CLOCK twin of @ref isPhaseFirstSighting.  Always
    /// true unless the emitter was constructed with @c emit_clock_disc (A2), which
    /// is what fills @c seen_clock_sats_.
    bool isClockFirstSighting(const std::string& sat) const;

    /**
     * @brief RTA-4: adopt serialized discontinuity-counter VALUES into this
     *        emitter's stream state (late-joining / tail-reader support).
     *
     * When a product file carries the optional per-record counter fields
     * (@c network.serialize_disc_counters) and the reader's replay recompute
     * disagrees — the expected case for a reader that starts mid-file, where every
     * key is a first sighting and recomputes to 0 — the reader calls this to
     * overwrite the emitter's counters with the file's authoritative post-emit
     * values, then re-emits the epoch so the store records carry them.  Keys use
     * the emitter's internal "sat|signal" form (@c clock keyed by bare sat id).
     * Only the COUNTERS are touched: the bias baselines / first-sighting markers /
     * satellite-set trackers were already correctly seeded by the preceding
     * @c emitToStore, so subsequent epochs recompute increments on top of the
     * adopted values exactly as the live producer would.
     *
     * @param phase Map "sat|signal" → phase-bias discontinuity counter.
     * @param code  Map "sat|signal" → code-bias discontinuity counter.
     * @param clock Map "sat"        → clock discontinuity counter.
     */
    void adoptDiscontinuityCounters(const std::map<std::string, int>& phase,
                                    const std::map<std::string, int>& code,
                                    const std::map<std::string, int>& clock);

    /**
     * @brief RTB-7: serialize the emitter's ENTIRE cross-epoch stream state to
     *        @p os as self-terminated, line-oriented text.
     *
     * Everything a warm-resumed producer needs to continue the discontinuity
     * bookkeeping EXACTLY where the bounced process left it: the phase/code/
     * clock discontinuity counters, the last-value baselines
     * (@c last_phase_bias_ / @c last_code_bias_), the first-sighting sets
     * (@c seen_clock_sats_ / @c prev_code_satset_ / @c prev_band_tuple_), the
     * RTA-11 last-seen timestamp maps, AND the RT0-5 pending forced-bump state
     * (@c force_all_bump_ / @c forced_bump_consts_ / @c forced_bump_sats_).
     * Restoring counters WITHOUT the baselines/first-sighting sets/pending
     * flags would emit wrong counters after resume (spurious first-sighting
     * resets, lost deferred datum bumps) — the silent consumer datum break
     * this accessor exists to prevent, so it is all-or-nothing by design.
     *
     * Format: an @c EMITTER,v1 header carrying the three construction
     * parameters (disc threshold, clock-disc flag, staleness horizon — a
     * restore into a differently-configured emitter must fail), one line per
     * map/set entry in deterministic key order, an @c EEND terminator.
     * Doubles use fmt's shortest round-trippable formatting (the
     * @c NetworkProductIO convention), so a parse recovers the exact value.
     *
     * The emitter knows nothing about files: the caller (the RTB-7 snapshot
     * module) owns framing, storage and atomicity.
     *
     * @param os Destination stream (any @c std::ostream — no flush performed).
     */
    void serializeState(std::ostream& os) const;

    /**
     * @brief RTB-7: restore stream state previously written by
     *        @ref serializeState, replacing this emitter's ENTIRE cross-epoch
     *        state (counters, baselines, first-sighting sets, RTA-11 last-seen
     *        stamps, RT0-5/B1 pending forced-bump flags).
     *
     * All-or-nothing: the input is parsed into staging state first and only a
     * COMPLETE, well-formed section (header through @c EEND) whose
     * construction parameters match this emitter's is committed.  On any
     * failure — truncation, malformed field, unknown tag, or a parameter
     * mismatch (the emitter was constructed from a different config than the
     * snapshotting producer's) — the emitter is left UNTOUCHED and false is
     * returned, which the caller treats as the cold-start signal.
     *
     * Distinct from @ref adoptDiscontinuityCounters (RTA-4), which overwrites
     * ONLY counter values on top of live-seeded baselines for a mid-file
     * product reader; this is a full state transplant for a producer resume.
     *
     * @param is Source stream positioned at the @c EMITTER header line.
     * @return True when the state was fully restored.
     */
    bool restoreState(std::istream& is);

private:
    double disc_threshold_m_;
    /// H3: when > 0, per-(sat,signal) jump threshold = disc_threshold_frac_cycles_ ×
    /// λ(sat,signal) (fixed contract, uniform in cycles).  0 = legacy flat metres.
    double disc_threshold_frac_cycles_{0.0};
    /// The jump-test threshold (m) for a given (sat, signal): frac·λ when the
    /// fractional-cycle mode is armed and λ resolves, else the flat metre gate.
    double discThresholdM(const std::string& sat, const std::string& signal) const;
    /// Card JT: screen the DE-COMMONED phase step (raw minus the signal's median)
    /// rather than the raw one.  Default false = the legacy frame-blind test
    /// (byte-identical).  Travels in the S contract (@c discdecom=).
    bool   disc_jump_test_decommon_{false};
    /// Card MS: bump only the channels a mark named.  Default false = the legacy
    /// satellite-wide scope (byte-identical).  Travels in the S contract
    /// (@c discscope=).  Declared here so the ctor's init list stays in
    /// declaration order.
    bool   disc_mark_scope_channel_{false};
    /// Card JT: the classified cells of the last emit (empty unless armed).  Per-emit
    /// scratch, deliberately NOT serialized: the de-commoned step is a pure function
    /// of this epoch's emitted biases and @ref last_phase_bias_, so a replay
    /// reproduces it with no extra stream state.
    std::vector<PbJumpDecommonEvent> pb_jump_decommon_events_;
    long long clv_jump_excluded_{0};   ///< card CLV: see clvJumpExcluded()
    bool   emit_clock_disc_{false};       ///< A2: maintain + stamp the clock disc counter.
    /// RTA-11: staleness horizon (s, PRODUCT time; 0 = off).  With it on, the
    /// last_seen maps below record each key's last-emitted epoch time; a key
    /// returning after a longer gap is treated as a first sighting (baseline
    /// reseed, no bump) on its channel.  The maps are only populated when the
    /// horizon is armed, so the OFF path carries no extra state.
    double staleness_horizon_s_{0.0};
    std::map<std::string, double> phase_last_seen_s_;  ///< "sat|signal" -> last emit product time (s).
    std::map<std::string, double> code_last_seen_s_;   ///< "sat|signal" -> last emit product time (s).
    std::map<std::string, double> clock_last_seen_s_;  ///< "sat" -> last emit product time (s).
    // Constellations to force-bump at the NEXT emit.  force_all_bump_ (set by an
    // empty-arg markDatumChange) forces EVERY constellation — the pre-A2 global
    // semantics; forced_bump_consts_ scopes the bump to specific constellations.
    // A constellation's flag is consumed only by an emit carrying at least one of
    // its satellites (see markDatumChange); a flagged constellation absent from an
    // emit (RT0-5 quarantine, all-set) stays pending here — a consumed global bump
    // demotes its absent-but-tracked constellations into forced_bump_consts_.
    bool           force_all_bump_{false};
    std::set<char> forced_bump_consts_;
    // Satellites to force-bump at the NEXT emit (B1 retro-bump, markSatDiscontinuity).
    // Per-satellite scoping (phase/code/clock counters of exactly these sats), distinct
    // from the per-constellation forced_bump_consts_ so one flagged satellite does not
    // reset a whole constellation at the consumer.  Cleared each emit.
    std::set<std::string> forced_bump_sats_;
    /// Card MS: the CHANNEL mask each marked satellite was marked with (a bit-or
    /// of @ref DiscChannel), kept in step with @ref forced_bump_sats_ and cleared
    /// with it.  Populated ONLY under @ref disc_mark_scope_channel_, so the legacy
    /// scope allocates nothing and the emit path never reads it.
    std::map<std::string, unsigned> forced_bump_chan_;
    /// Card MS: per-emit scratch — the marks this emit withheld from a channel,
    /// and the marks it applied.  Both empty/zero under the legacy scope.
    std::vector<MarkScopeEvent> mark_scope_events_;
    MarkChannelTally            mark_channel_tally_{};
    std::map<std::string, int>    disc_counter_;    ///< "sat|signal" -> phase discontinuity counter.
    std::map<std::string, double> last_phase_bias_; ///< "sat|signal" -> last emitted phase bias (m).

    // --- Satellite CLOCK discontinuity bookkeeping (A2; only when emit_clock_disc_) ---
    std::map<std::string, int> clock_disc_counter_; ///< "sat" -> clock discontinuity counter.
    std::set<std::string>      seen_clock_sats_;    ///< sats whose clock was emitted at least once (first-sighting marker).

    // --- Satellite code-bias discontinuity bookkeeping (H1) ---------------
    // prev_code_satset_ / prev_band_tuple_ are WHOLESALE-REPLACED with the
    // current epoch's maps inside emitToStore (never merged): a (const,signal)
    // channel or a satellite that disappears and later reappears is then a
    // clean first-sighting on both a live run AND a fresh replay process, so
    // the counters reproduce deterministically.  They update ONLY on an actual
    // emit (the writer persists only emitted epochs; replay must reproduce the
    // emitted-only diff sequence).
    std::map<std::string, int>    code_disc_counter_; ///< "sat|signal" -> code discontinuity counter.
    std::map<std::string, double> last_code_bias_;    ///< "sat|signal" -> last emitted code bias (m); first-sighting marker.
    std::map<std::string, std::set<std::string>> prev_code_satset_; ///< "const|signal" -> sats carrying it last emit.
    std::map<std::string, std::vector<char>>     prev_band_tuple_;  ///< "sat" -> sorted distinct code band-digits last emit (IF-pin proxy).

    // --- Optional discontinuity channels (both default OFF = byte-identical) --
    bool code_bias_jump_test_{false};      ///< Also bump the code counter on a fresh-baseline value jump > disc_threshold_m_.
    bool clock_disc_on_set_change_{false}; ///< Sum-to-zero datum: bump a constellation's clock counters when its clock-sat SET changes between emits.
    /// Constellation char -> sats whose clock was emitted last emit.  WHOLESALE-
    /// REPLACED each emit (mirrors prev_code_satset_); populated ONLY when
    /// clock_disc_on_set_change_ is armed, so the OFF path carries no extra state
    /// and the serialized emitter section is unchanged.
    std::map<char, std::set<std::string>> prev_clock_satset_;
};

} // namespace GnssProcessor
