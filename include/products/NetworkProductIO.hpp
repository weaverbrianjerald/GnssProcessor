#pragma once
/**
 * @file NetworkProductIO.hpp
 * @brief On-disk (de)serialization of the PPP-RTK network joint solution.
 *
 * Sink A of the network product-emit path (@c NetworkProductEmitter) pushes each
 * epoch's estimated satellite-side solution into an in-memory
 * @c SsrCorrectionStore for the in-process closed-loop consumer.  This module
 * adds the persistent half: a writer that appends each @c NetworkSolutionEpoch
 * to a plain-text product file as the producer runs, and a reader that parses it
 * back, so consumer-side experiments (for example the consumer AR-frame work) can
 * replay the products without re-running the multi-station network.
 *
 * The file holds the pre-emit @c NetworkSolutionEpoch (estimable satellite clock
 * and code/phase bias) plus the per-epoch @c datum_changed flag, the one piece of
 * stream state the epoch struct does not carry (the driver sets it on a
 * clock-pivot handover).  By default the discontinuity counters are not
 * serialized: they are stream state owned by @c NetworkProductEmitter and are
 * recomputed deterministically on replay.  Feeding the parsed records back
 * through the same @c NetworkProductEmitter::emitToStore (via @c replay, which
 * also re-applies @c markDatumChange) therefore reproduces the producer's live
 * @c SsrCorrectionStore contents byte-for-byte, discontinuity counters included.
 * With @c network.serialize_disc_counters on (RTA-4), the post-emit counter values
 * are also written as optional trailing fields, so a late-joining or tail reader
 * can reconstruct them without replaying from epoch 1; @c replay still recomputes
 * them as a self-check and adopts the file values on disagreement.
 *
 * File format (line-based, '#' comments, comma-separated; one epoch is an @c E
 * line followed by its per-satellite @c C / @c B / @c P records):
 * @code
 *   # gnss_network_product v1
 *   E,<week>,<tow>,<iod_ssr>,<provider_id>,<solution_id>,<datum_changed 0|1>
 *   C,<sat>,<clock_c0_m>                              # satellite clock (has_clock)
 *   B,<sat>,<signal>,<code_bias_m>                    # satellite code bias
 *   P,<sat>,<signal>,<bias_m>,<signal_integer 0|1>,<wide_lane_integer>
 * @endcode
 * Optional records and fields (each present only when its producer toggle is on,
 * so a run with the toggles off writes the base format verbatim):
 * @code
 *   I,<generation_id>                       # RTA-3 solution-generation id (after E/G)
 *   C,<sat>,<c0>[,<c1>][,<clock_disc>]      # RTA-4: with counters on, c1 is always
 *     [,iode=<n>][,oiod=<n>]                #   written (0 when absent) so the clock
 *     [,oepoch=<week>:<tow>]                #   disc counter sits at fixed field 5.
 *                                           #   Card 4 (RTF-2): the iode=/oiod=/
 *                                           #   oepoch= key=value tokens carry the
 *                                           #   brdc_rts broadcast-IODE anchor and RTS
 *                                           #   orbit echo (all absent in sp3 mode;
 *                                           #   a field containing '=' ends the
 *                                           #   positional c1/clock_disc parse)
 *   B,<sat>,<signal>,<bias>[,<code_disc>]   # RTA-4 code disc counter (field 5)
 *   P,<sat>,<sig>,<b>,<si>,<wl>[,<ph_disc>] # RTA-4 phase disc counter (field 7)
 *     [,sigma=<m>][,sigc=<m>]               # B1b phase-bias σ (sigma=) and M4 combined
 *                                           #   phase-correction σ (sigc=; clock+phasebias)
 *   C,...[,sigcc=<m>][,rho=<val>]           # M4 per-sat combined code-correction σ
 *                                           #   (sigcc=) and code/phase correlation (rho=)
 *   S,...[,gid=<id>][,stale=<sec>]          # RTA-3/RTA-11 trailing key=value fields;
 *     [,frame=<id>][,cepoch=<epoch>]        #   RTA-12 contract identity fields
 *     [,agek=<val>]                         #   M4 combined-σ age-scaling coefficient
 *     [,atx=<file>][,oref=<CoM|APC>]        #   (each absent when empty);
 *     [,cbpin=<const>:<s1>-<s2>...]         #   §1.5 code-bias IF pin pair per
 *                                           #   constellation (the pair the OSB
 *                                           #   stream is internally consistent in)
 *     [,gfpin=<const>:<s1>-<s2>...]         #   Odijk rd 3b: the geometry-free datum
 *                                           #   pair, present only in the slant
 *                                           #   posture (network.code_bias_gf_datum).
 *                                           #   When present, the pinned pair's
 *                                           #   B records are the datum (0) and the
 *                                           #   GF lump lives in the ionosphere
 *     [,sat_gf_datum=<mode>]                #   RF-0d: the value that datum was
 *                                           #   pinned at, zero | broadcast_tgd.
 *                                           #   A separate token from gfpin=; the
 *                                           #   two frames differ by c*T_GD on
 *                                           #   every code bias and by the opposite
 *                                           #   shift in the ionosphere.
 *                                           #   Repeated S lines mid-file are RTA-5
 *                                           #   contract re-broadcasts (positional)
 *   H,<week>,<tow>,<status>,<sigma0>,       # RTA-10 epoch-status heartbeat
 *     <n_sats_published>,<withheld>         #   (network.emit_epoch_status_record)
 * @endcode
 * The @c H heartbeat (RTA-10) is positional, like the S contract line.  It is
 * written immediately before the epoch block it describes (status @c ok /
 * @c partial), or stands alone with no E line for a fully withheld epoch (status
 * @c withheld_sigma0 / @c withheld_gate).  @c withheld is "*" for a fully withheld
 * epoch, "-" on a clean emit, and otherwise the semicolon-joined constellation
 * letters.  @c sigma0 is the global within-epoch σ̂0 ("%.4f"; 0 when unavailable).
 * Like S, H never feeds @c replay: the same file without its H lines reconstructs
 * a byte-identical store.  Format-version note: a pre-RTA-10 reader treats @c H as
 * an unknown tag; it still parses every epoch correctly but sets its sticky
 * @c ok / @c healthy() to false.  Arm @c network.emit_epoch_status_record only when
 * the consumers reading the file are current.  The '#' header legend is
 * grammar-complete (Handoff-§7 schema fix): one line per record tag, declaring
 * every optional positional field and key=value token the writer can emit, shared
 * verbatim by v1 and v2 headers.  A wire-format extension must extend the legend;
 * the test @c HeaderLegendDeclaresEveryEmittedField enforces this.
 * Floating-point fields are written with the shortest round-trippable decimal
 * (fmt's default formatting), so a parse recovers the exact @c double.  A
 * satellite with no @c C line round-trips to @c has_clock=false; a satellite with
 * no records at all is dropped (it would push nothing through @c emitToStore, so
 * the store result is identical).
 */

#include "products/NetworkProductEmitter.hpp"   // NetworkSolutionEpoch, NetworkProductEmitter
#include "products/SsrCorrectionStore.hpp"      // SsrCorrectionStore, NetworkProducerStatus (RTA-10)
#include "products/SignalDeclaration.hpp"       // NetworkSignalDeclaration (card SID phase 5)
#include "filter/NetworkSbasis.hpp"             // SbasisDeclaration (B6 contract)

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/// S-basis contract stamped into a v2 product header (the "S" line), so a consumer
/// can check compatibility before claiming integer recovery (B6).  Holds the S-basis
/// convention (@ref SbasisDeclaration), the identity of the a-priori orbit/clock
/// product the emitted clock C0 is a delta against, and the phase-bias
/// discontinuity threshold the emitter used.  @c present is true only when a v2
/// header carried it (a v1 file or a run with the contract off leaves it default).
struct SbasisContract {
    SbasisDeclaration decl;                   ///< The realized S-basis convention.
    std::string       apriori_product_id;     ///< A-priori orbit/clock product identity, e.g. "CODFIN2024129".
    /// Phase-bias discontinuity threshold the emitter used.  The default is the
    /// emitter's own single constant (R9); @c makeReplayEmitter applies it to a v1
    /// file with no S line, so it must track the live emitter default.
    double            disc_threshold_m{NetworkProductEmitter::kDefaultDiscThresholdM};
    /// Per-λ fractional-cycle discontinuity threshold
    /// (@c network.disc_threshold_frac_cycles; H3).  When > 0, the emitter's phase
    /// (and opt-in code) jump test uses @c frac·λ(sat,signal) instead of the flat
    /// @c disc_threshold_m.  Sixth replay-emitter construction parameter.
    /// Serialized as an optional trailing @c discfrac= S-line field only when > 0,
    /// so a flat-metre producer writes the S line byte-for-byte as before.
    double            disc_threshold_frac_cycles{0.0};
    /// Whether the producer's phase jump test screened the de-commoned step (raw ΔPB
    /// minus the median ΔPB over the satellites emitted on that signal at both
    /// epochs) rather than the raw one (@c network.disc_jump_test_decommon, card JT).
    /// Seventh replay-emitter construction parameter.  A consumer replaying with the
    /// other setting miscounts discontinuities on the cells where a frame move hid or
    /// created a step, so @c makeReplayEmitter applies it and @c checkContract
    /// compares it.  Serialized as an optional trailing @c discdecom=1 S-line field
    /// only when armed, so a producer without it writes the S line byte-for-byte.
    bool              disc_jump_test_decommon{false};
    /// Whether a per-satellite discontinuity mark bumped only the generations of the
    /// channels the marking site reported, rather than the satellite's clock,
    /// code-bias and phase-bias generations together (@c network.disc_mark_scope,
    /// card MS).  Eighth replay-emitter construction parameter.  Serialized as an
    /// optional trailing @c discscope=channel S-line field only when armed, so a
    /// producer without it writes the S line byte-for-byte.
    /// The @c R record carries satellite ids and no channel set, so a replay
    /// re-marks with the full satellite scope whatever this field says;
    /// @c checkContract compares it so the divergence is reported.
    bool              disc_mark_scope_channel{false};
    /// Whether the producer held each (satellite, signal) emitted phase bias at the
    /// first value of its generation (@c network.emit_pb_hold, card PBH).  Not a
    /// replay-emitter parameter but a reading rule for the whole phase-bias channel:
    /// when held, a @c P record's @c bias_m is a generation constant rather than a
    /// per-epoch estimate, each record carries a @c gen= token, and a @c phase_disc
    /// bump without a @c gen= change carries no value step, so a consumer re-floats
    /// on @c gen= and ignores the counter.  Serialized as an optional trailing
    /// @c pbhold=1 S-line field only when armed, so a producer without it writes the
    /// S line byte-for-byte.  @c checkContract compares it in both directions, since
    /// a @c gen= payload under a @c pbhold=0 contract (or the reverse) means the
    /// file's contract and records disagree.
    bool              emit_pb_hold{false};
    /// Whether the hold keeps the phase-visible clock instead of the phase bias
    /// (@c network.emit_pb_hold_phase_clock, card LAH): within a generation a held
    /// @c bias_m moves with the satellite's common-removed clock, so @c clock - bias is
    /// the generation constant and a clock move reaches the code side only.  Serialized
    /// as @c pbhold=2 in place of @c pbhold=1 (a reader that tests @c pbhold != 0 still
    /// reads a hold); @c checkContract compares it.  Never true without
    /// @c emit_pb_hold.
    bool              emit_pb_hold_phase_clock{false};
    /// Whether the hold is the smoothed member (@c network.emit_pb_hold_smoothed, card
    /// SMH): a held @c bias_m follows the phase-clock transfer minus the clock clone's
    /// passed motion within a generation.  Serialized as @c pbhold=3; @c checkContract compares it.  Never true
    /// without @c emit_pb_hold.
    bool              emit_pb_hold_smoothed{false};
    /// Whether the smoothed hold is its phase-visible member
    /// (@c network.emit_pb_hold_smoothed_block, card SMD): the held @c bias_m follows the
    /// phase-clock transfer minus the block clone's phase-visible passed motion.  Serialized
    /// as @c pbhold=4 (which also sets @c emit_pb_hold_smoothed on read);
    /// @c checkContract compares it.  Never true without @c emit_pb_hold_smoothed.
    bool              emit_pb_hold_smoothed_block{false};
    /// Whether the producer's emitter maintained the per-satellite clock
    /// discontinuity counter (A2).  Second replay-emitter construction parameter;
    /// with the others in this struct it determines the @c NetworkProductEmitter
    /// construction, so a consumer can replay the file with byte-identical
    /// discontinuity counters and no out-of-band knowledge of the producer's config
    /// (see @c makeReplayEmitter).
    bool              emit_clock_disc{false};
    /// The emitter's staleness horizon (s, product time; 0 = off; RTA-11).  Third
    /// replay-emitter construction parameter (see @c makeReplayEmitter).
    /// Serialized as an optional trailing @c stale= field on the S line only when
    /// positive, so a run with it off writes the pre-RTA-11 v2 header byte-for-byte.
    double            staleness_horizon_s{0.0};
    /// Age-scaling coefficient the consumer uses to inflate the combined-correction
    /// variance as σc²·(1 + agek·Δt_age²) (Δt_age = correction content age, s),
    /// replacing the ad-hoc URA staleness ramp for network-corrected rows (M4,
    /// contract v2).  From the producer's @c network.combined_sigma_agek.  Serialized
    /// as an optional trailing @c agek= S-line field only when non-zero, so a
    /// producer that does not declare it writes a byte-identical S line.  A quality
    /// hint, not a replay-emitter parameter: @c makeReplayEmitter ignores it and
    /// @c checkContract does not compare it.
    double            age_scale_k{0.0};
    /// Whether the producer's emitter ran the opt-in code-bias magnitude jump test
    /// (@c network.code_bias_disc_jump_test).  Fourth replay-emitter construction
    /// parameter.  Serialized as an optional trailing @c cbjump=1 field only when
    /// true (byte-identical when off).
    bool              code_bias_jump_test{false};
    /// Whether the producer's emitter bumped clock disc counters on a constellation
    /// clock-set change (@c network.clock_disc_on_set_change, the sum-to-zero
    /// datum-move signal).  Fifth replay-emitter construction parameter.  Serialized
    /// as an optional trailing @c clkset=1 field only when true (byte-identical when
    /// off).
    bool              clock_disc_on_set_change{false};
    /// Which statistic the H heartbeat's sigma0 field carries (H-rider, edge-queue
    /// §1.6 item 3a).  Empty = the RTA-10 default (the within-epoch RAIM's σ̂₀, 0
    /// when that tier did not run); "pc" = the always-on prefit census's pooled
    /// robust σ̂₀ (@c network.h_sigma0_prefit_census), a pre-solve statistic with the
    /// station common mode removed.  The two are different populations, so the wire
    /// declares which one it carries.  Serialized as an optional trailing @c hsig=
    /// field only when non-empty (byte-identical when off).  An interpretation hint,
    /// not a replay-emitter parameter: @c makeReplayEmitter ignores it and
    /// @c checkContract does not compare it.
    std::string       h_sigma0_source;
    /// The producer's solution-generation id (0 = not stamped; RTA-3).  Serialized
    /// as an optional trailing @c gid= field on the S line only when non-zero, so a
    /// run with it off writes a byte-identical v2 header.  Lets a late joiner recover
    /// the generation from the contract line alone (re-broadcast mid-file, RTA-5).
    uint32_t          generation_id{0};
    // --- RTA-12 contract identity fields --------------------------------------
    // Each serializes as an optional trailing key=value S-line field only when
    // non-empty (the gid=/stale= pattern), so a producer that declares none of
    // them writes a byte-identical S line.  checkContract compares each one only
    // when the consumer expectation declares it non-empty (the
    // apriori_product_id semantics).
    /// Reference frame the fixed station coordinates are expressed in, e.g.
    /// "IGS20" (config declaration @c network.frame_id; @c frame= field).
    std::string       frame_id;
    /// Epoch the station coordinates are valid at: a decimal year ("2024.5") or
    /// an ISO date ("2024-07-01").  From @c network.coord_epoch, or defaulted to
    /// the SIF propagation date under @c network.use_sif_coordinates
    /// (@c cepoch= field).
    std::string       coord_epoch;
    /// Basename of the antenna-calibration (ATX) file the producer run loaded,
    /// e.g. "igs20.atx"; empty when unresolvable (@c atx= field).
    std::string       atx_id;
    /// Orbit reference point of the a-priori orbit/clock products: "CoM"
    /// (center of mass; the SP3+ATX convention) or "APC" (antenna phase center;
    /// SSR orbit streams).  @c oref= field.
    std::string       orbit_ref;
    /// Code-bias IF pin pair per constellation: the signal pair the emitted OSB
    /// stream is internally consistent in, keyed by constellation letter with value
    /// "<s1>-<s2>" (e.g. "G" → "C1C-C2L", the pair @c selectIfPinPair realized).
    /// See §1.5 of the day-129 frame-guards report.  The producer's code-bias datum
    /// pins IF(s1,s2) per satellite plus sum-to-zero per (constellation, signal)
    /// channel; IF combinations outside the pinned pair follow data-determined
    /// directions and carry the full frame wander.  A consumer should form its IF
    /// within one frame.  The measured ranking is pinned pair < convention pair <
    /// mixed pair (one signal in, one out); the mixed pair is worst because the
    /// consumer's receiver code-bias state must absorb a between-channel
    /// differential against the moving satellite OSBs.  Serialized as repeated
    /// trailing @c cbpin=<const>:<s1>-<s2> S-line fields only when non-empty
    /// (byte-identical otherwise).  An interpretation hint, not a replay-emitter
    /// parameter: @c makeReplayEmitter ignores it and @c checkContract compares it
    /// only for constellations the consumer declares (the @c clock_pivot_sat
    /// semantics).
    std::map<std::string, std::string> cb_pin_pair;
    /// Per-constellation geometry-free datum pair (@c network.code_bias_gf_datum,
    /// Odijk rd 3b).  Present only when the producer folded a GF row per satellite in
    /// addition to the IF pin (the "slant posture": free per-LOS ionosphere, no SH
    /// field, no GIM).  The value is @c "<s1>-<s2>" and is currently always the same
    /// pair as @ref cb_pin_pair (the GF row is built on the IF pin's post-hold
    /// @c sig1/@c sig2).  It is carried explicitly rather than implied so the token is
    /// self-contained and so the per-channel-family form (@c "C1C-C5Q+C1X-C5X", see
    /// @c docs/SPEC_family_gf_datum.md) is a value extension rather than a new token.
    /// The parser already accepts that '+'-joined form; this producer writes one pair
    /// per constellation.
    ///
    /// Why the pair is on the wire: under the GF datum, IF+GF together span the
    /// pinned pair's whole 2-D subspace, so those two channels' B records are
    /// identically the datum (0) rather than estimates, only j >= 3 carries content,
    /// and each satellite's geometry-free bias has moved into the slant ionosphere
    /// (Odijk Table 8).  That is a different estimable function from the
    /// single-layer posture on a byte-identical record layout.  A consumer or
    /// truth-grader that applies the single-layer S-transform to a GF product gets
    /// wrong results without any error, and no other field on the S line
    /// distinguishes the two (@c cb_datum is a single-valued enum).  The ionosphere
    /// half of that pair is not serialized anywhere: the consumer estimates its own,
    /// which this token tells it to do.
    ///
    /// Serialized as repeated trailing @c gfpin=<const>:<s1>-<s2> S-line fields only
    /// when non-empty (byte-identical otherwise).  An interpretation hint, not a
    /// replay-emitter parameter: @c makeReplayEmitter ignores it (the datum does not
    /// change how emitted values reconstruct into the store) and @c checkContract
    /// compares it only for constellations the consumer declares (the
    /// @c clock_pivot_sat / @ref cb_pin_pair semantics).
    std::map<std::string, std::string> gf_pin_pair;
    /// The value the hard per-satellite geometry-free datum was pinned at:
    /// `"zero"` or `"broadcast_tgd"`; empty when the producer folded no such datum
    /// (`network.sat_gf_datum`, RF-0d).
    ///
    /// A separate field, not an overload of @ref gf_pin_pair.  That token's payload
    /// is its presence ("the geometry-free direction is a datum, not an estimate")
    /// and it carries a signal pair; this one states what the datum was pinned at.
    /// The two modes differ by `c*T_GD` on every emitted code bias of the satellite,
    /// with the opposite shift in the emitted ionosphere (Odijk Table 8: the pinned
    /// lump moves there).  Carrying a mode in `gfpin=`'s value grammar would give one
    /// token two unrelated meanings and break the existing parser's `<const>:<pair>`
    /// contract; a consumer that does not know `sat_gf_datum=` skips it as an
    /// unknown field, as the grammar intends.
    ///
    /// Serialized as a single trailing @c sat_gf_datum=<mode> S-line field only when
    /// non-empty, so a producer without it writes the pre-RF-0d S line byte for byte.
    /// An interpretation hint, not a replay-emitter parameter: @c makeReplayEmitter
    /// ignores it (the datum does not change how emitted values reconstruct into the
    /// store) and @c checkContract does not compare it (a consumer estimating its
    /// own ionosphere is consistent under either mode; the absolute interpretation
    /// of the pair differs, which matters to a grader and to anyone combining this
    /// product with an external DCB).
    std::string       sat_gf_datum;
    bool              present{false};         ///< True when parsed from (or written to) a v2 header.
};

/// One serialized network-product record: the epoch's solution plus the
/// driver-side @c datum_changed flag (a clock-pivot handover this epoch), which
/// @c NetworkProductReader::replay turns back into a forced discontinuity bump.
struct NetworkProductRecord {
    NetworkSolutionEpoch epoch;                 ///< The estimable satellite-side solution.
    bool                 datum_changed{false};  ///< Clock-pivot / S-basis re-declaration this epoch.
    /// Constellations whose clock datum re-referenced this epoch (A2 per-
    /// constellation scoping).  Serialized as E-line field 8 only when non-empty;
    /// an empty set (old-format files, or a run with per-constellation scoping off)
    /// makes @c replay force a global bump, as before A2.
    std::set<char>       datum_consts;
    /// Satellites the B1 retro-bump flagged for a per-satellite discontinuity this
    /// epoch (a post-emit QC tier caught contamination the previous epoch).
    /// Serialized as a separate @c R record (comma-separated sat ids) only when
    /// non-empty, so a run with the retro-bump off writes the pre-B1 format
    /// verbatim; @c replay turns it back into @c markSatDiscontinuity.
    std::set<std::string> retro_sats;
    /// Discontinuity-counter values parsed from the optional trailing fields
    /// (C field 5 / B field 5 / P field 7; present only when the producer ran with
    /// @c network.serialize_disc_counters, RTA-4).  They are the post-emit counters
    /// of this epoch, identical to what the producer's @c emitToStore pushed into its
    /// live store.  @c replay keeps its deterministic recompute and uses these as a
    /// self-check: on any disagreement (the expected case for a reader that joined
    /// mid-file) it warns, adopts the values into the replay emitter and re-stamps
    /// the store records.  Empty maps for old files and runs with the toggle off.
    struct SerializedDiscCounters {
        std::map<std::string, int> clock;  ///< "sat"        -> clock disc counter (C field 5).
        std::map<std::string, int> code;   ///< "sat|signal" -> code disc counter  (B field 5).
        std::map<std::string, int> phase;  ///< "sat|signal" -> phase disc counter (P field 7).
        bool any() const { return !clock.empty() || !code.empty() || !phase.empty(); }
    };
    SerializedDiscCounters serialized_disc;
    /// The S-contract state as of this epoch (RTA-5): set (present=true) when an
    /// S line immediately preceded this epoch's E line (the deferred v2 header S
    /// counts for the first epoch; a mid-file re-broadcast counts for the epoch it
    /// precedes).  A consumer that declares an expectation should run
    /// @c checkContract on every present refresh, not once at end of file.  S
    /// lines never feed @c replay, so replay determinism is unaffected.
    SbasisContract contract_refresh;
    /// This epoch's status heartbeat (RTA-10): set when an H line with matching
    /// week/tow immediately preceded this epoch's E line (the producer writes the
    /// emitted epoch's H right before its block).  A standalone H (fully withheld
    /// epoch) attaches to no record; it surfaces through
    /// @c NetworkProductTail::lastStatus instead.  H lines never feed @c replay.
    std::optional<NetworkProducerStatus> epoch_status;
    /// The producer's declared signal roster as of this epoch (card SID phase 5,
    /// E2): set when an @c N declaration block immediately preceded this epoch's E
    /// line.  A declaration replaces the previous one wholesale, so the record
    /// carries the whole roster, not a delta.  @c replay pushes it into the consumer
    /// store, where it is the only information that lets a freshness flip
    /// distinguish "the network carries no family for this signal" from "the record
    /// is stale".  Absent on wires written before card SID.
    std::optional<NetworkSignalDeclaration> declaration_refresh;
};

/**
 * @class NetworkProductWriter
 * @brief Appends @c NetworkSolutionEpoch records to a product file.
 *
 * Opens the file (truncating) and writes the format header on construction, then
 * appends one block per @c write call.  The stream is flushed/closed when the
 * writer is destroyed.  Check @c good() after construction; a failed open leaves
 * the writer inert (every @c write is a no-op) so the producer can degrade
 * gracefully rather than abort.
 */
class NetworkProductWriter {
public:
    /// Open @p path for writing (truncate) and emit the v1 format header.
    explicit NetworkProductWriter(const std::string& path);

    /// Open @p path and emit a v2 header carrying @p contract (S-basis declaration,
    /// a-priori product identity, disc threshold) before any epoch (B6).  The
    /// per-epoch E/C/B/P records are unchanged; only the header differs from v1.
    NetworkProductWriter(const std::string& path, const SbasisContract& contract);

    /// True iff the output stream is open and healthy.
    bool good() const { return static_cast<bool>(out_); }

    /// Path the writer was constructed with (for diagnostics).
    const std::string& path() const { return path_; }

    /// Append one epoch.  @p datum_changed records a clock-pivot handover this
    /// epoch so a replay can reproduce the forced discontinuity bump.
    /// @p datum_consts (A2) is the set of constellations whose clock datum
    /// re-referenced; it is appended as E-line field 8 only when non-empty, so a
    /// run with per-constellation scoping off writes the pre-A2 format verbatim.
    /// @p retro_sats (B1) is the set of satellites the retro-bump flagged this
    /// epoch; it is written as a separate @c R record only when non-empty, so a run
    /// with the retro-bump off writes the pre-B1 format verbatim.
    /// @p disc_source (RTA-4, @c network.serialize_disc_counters): when non-null,
    /// each C/B/P record gains an optional trailing field carrying the current
    /// (post-emit) discontinuity counter queried from this emitter.  The caller
    /// must pass the emitter whose @c emitToStore just processed @p sol, so the
    /// serialized values are identical to what the store received for this epoch.
    /// On the C line the optional C1 drift field is then always written (0 when
    /// absent) so the clock counter sits at a fixed position.  nullptr (default)
    /// writes the pre-RTA-4 field counts verbatim.
    /// No-op if the stream is not @c good().
    void write(const NetworkSolutionEpoch& sol, bool datum_changed = false,
               const std::set<char>& datum_consts = {},
               const std::set<std::string>& retro_sats = {},
               const NetworkProductEmitter* disc_source = nullptr);


    /// Append a current machine-parseable "S" contract line mid-file (the periodic /
    /// datum-change contract re-broadcast, RTA-5).  Same grammar as the header S line,
    /// including the optional @c gid= / @c stale= trailing fields.  The caller writes
    /// it before the epoch block it describes, so a positional reader updates its
    /// contract state for the epochs that follow.  No-op if the stream is not
    /// @c good().
    void writeContract(const SbasisContract& contract);

    /// Append one positional epoch-status heartbeat line (RTA-10;
    /// @c H,week,tow,status,sigma0,n_sats_published,withheld, see the file banner).
    /// The producer driver writes it before the epoch block on an emit (as for the S
    /// line) and standalone at the gate / σ̂0 withhold sites, which otherwise emit
    /// nothing, so a live tail can distinguish "withheld" from "producer died".  An
    /// empty @p status.withheld is written as "-".  The caller owns the per-epoch
    /// flush (@c network.product_flush_per_epoch), as for epoch blocks.  No-op if
    /// the stream is not @c good().
    void writeEpochStatus(const NetworkProducerStatus& status);

    /// Append the producer's declared signal roster as a contiguous block of @c N
    /// records (card SID phase 5, E2; see @c products/SignalDeclaration.hpp for the
    /// grammar).  The caller writes it before the epoch block it describes, as for
    /// the S line, and only when the declaration changed, so an unarmed producer
    /// writes nothing and the wire is byte-identical.  The serializer skips a row
    /// with nothing to declare, so a declaration whose rows are all empty writes no
    /// lines.  No-op if the stream is not @c good().
    ///
    /// @param decl The declaration; @c present false writes nothing.
    /// @return The number of records written.
    std::size_t writeSignalDeclaration(const NetworkSignalDeclaration& decl);

    /// Flush the output stream to disk (RTA-6).  The producer driver calls this once
    /// per epoch under @c network.product_flush_per_epoch so a live tail consumer
    /// (@ref NetworkProductTail) sees each epoch block as soon as it is written
    /// rather than at ofstream-buffer boundaries (without it, an epoch block larger
    /// than the stream buffer reaches disk in arbitrary partial chunks, and a smaller
    /// one possibly minutes late).  Flushing changes no bytes (the file is identical
    /// with or without it), but a live tail needs it for bounded product latency.
    /// No-op if the stream is not @c good().
    void flush() { if (out_) out_.flush(); }

private:
    /// Emit the format banner + legend; when @p contract is non-null, a v2 banner
    /// plus the machine-parseable "S" contract line (B6).  Called by both ctors.
    void writeHeader_(const SbasisContract* contract);

    /// Serialize one "S" contract line (shared by the v2 header and the RTA-5
    /// mid-file re-broadcast so the two grammars cannot diverge).
    void writeSLine_(const SbasisContract& contract);

    std::string   path_;
    std::ofstream out_;
};

/**
 * @class NetworkProductReader
 * @brief Parses a product file and replays it into an @c SsrCorrectionStore.
 */
class NetworkProductReader {
public:
    /**
     * @brief Parse every epoch from @p path.
     * @param path Product file written by @c NetworkProductWriter.
     * @param ok   Optional out-flag: set false on an open failure or any
     *             malformed line (parsing continues past a bad line), true
     *             otherwise.
     * @return Records in file order (epoch order).  Empty if the file cannot be
     *         opened.
     */
    static std::vector<NetworkProductRecord> read(const std::string& path,
                                                  bool* ok = nullptr);

    /// Overload that also returns the parsed S-basis contract (the v2 "S" header
    /// line; B6).  @p contract_out->present is false for a v1 file or an absent "S"
    /// line (backward-compatible: @p ok stays true and replay is unaffected).
    ///
    /// @p statuses_out, when non-null, is filled with every H heartbeat in file
    /// order (H-rider, edge-queue §1.6 item 3b).  This includes the standalone H
    /// lines of fully withheld epochs, which complete no product record and so
    /// appear neither in the returned records nor in the per-record
    /// @c epoch_status field.  This is the batch-read analogue of
    /// @c NetworkProductTail::lastStatus (the live surface).  H still never feeds
    /// @c replay; the caller decides what to do with the statuses (the
    /// consumer-replay driver forwards them to
    /// @c SsrCorrectionStore::setProducerStatus, mirroring the RT service loop).
    static std::vector<NetworkProductRecord> read(const std::string& path,
                                                  SbasisContract* contract_out,
                                                  bool* ok,
                                                  std::vector<NetworkProducerStatus>* statuses_out = nullptr);

    /// Cross-check a file's S-basis contract against the consumer's @p expected
    /// convention (B6).  Returns true if compatible; on any mismatch returns false
    /// and (if @p mismatches non-null) appends a human-readable line per differing
    /// field.  The caller decides whether a mismatch is a warning or a hard error.
    static bool checkContract(const SbasisContract& file, const SbasisContract& expected,
                              std::vector<std::string>* mismatches = nullptr);

    /// Replace every ',' with ';' so an arbitrary string is safe to write into a
    /// single CSV field of @c network_contract_check.csv (L14).  Defensive: the
    /// current checkContract mismatch strings are comma-free, but this keeps the
    /// check output robust to future message edits.
    static std::string csvSanitize(const std::string& s);

    /// Construct an emitter configured as the producer that wrote @p contract, from
    /// the eight replay-emitter construction parameters the contract carries
    /// (@c disc_threshold_m, @c emit_clock_disc, @c staleness_horizon_s,
    /// @c code_bias_jump_test, @c clock_disc_on_set_change,
    /// @c disc_threshold_frac_cycles, @c disc_jump_test_decommon,
    /// @c disc_mark_scope_channel), so a replay reproduces every discontinuity
    /// counter, the clock channel included, byte-for-byte with no out-of-band
    /// knowledge of the producer's config.  A v1 file (@c contract.present false)
    /// yields the default emitter, reproducing the pre-B6 replay behavior.  Feed the
    /// same instance to every @c replay call of one file (the emitter carries the
    /// cross-epoch counter state).
    static NetworkProductEmitter makeReplayEmitter(const SbasisContract& contract);

    /**
     * @brief Whether a consumer may replay a wire that carries this contract
     *        (card OWD, O1).
     *
     * The S contract line is part of every wire written since card OWD: it carries
     * the replay emitter's construction (@ref makeReplayEmitter), the
     * clock-discontinuity channel included.  A wire without it replays with the
     * default emitter, and no clock re-reference the producer declared reaches the
     * replay.  Such a wire (written before card OWD, when the line was optional) is
     * refused unless the consumer requests it through
     * @c network.replay_allow_undeclared_wire, the switch for grading a legacy leg
     * deliberately.
     *
     * @param file             The contract parsed from the wire (present or not).
     * @param allow_undeclared The consumer's @c network.replay_allow_undeclared_wire.
     * @param why              Optional out: empty for a declared wire; otherwise the
     *                         reason, naming the cause and (when refused) the two fixes.
     * @return true when the wire carries a contract, or when it does not and
     *         @p allow_undeclared is set; false otherwise.
     */
    static bool admitsWire(const SbasisContract& file, bool allow_undeclared,
                           std::string* why = nullptr);

    /**
     * @brief Replay one record into @p store via @p emitter, reproducing the live
     *        producer path: a @c markDatumChange (when @c rec.datum_changed) then
     *        @c emitToStore.  The consumer interleaves this with its own per-epoch
     *        processing as @c runNetwork does live, so the store always reflects the
     *        just-replayed epoch.
     */
    static void replay(const NetworkProductRecord& rec,
                       SsrCorrectionStore& store,
                       NetworkProductEmitter& emitter);
};

/**
 * @class NetworkProductTail
 * @brief Incremental ("tail -f") reader for a growing network product file (RTA-6).
 *
 * A standalone real-time consumer follows the producer's product file while the
 * producer is still appending to it.  Each @ref poll consumes the complete new
 * lines that have appeared since the last poll and returns the newly completed
 * epochs, each exactly once; @ref pollInto also replays every completed epoch into
 * a caller-supplied @c SsrCorrectionStore through the @c NetworkProductReader::replay
 * path the batch reader uses (one shared code path, including the RTA-4
 * serialized-counter adoption), driven by a replay emitter the tail owns, which
 * carries the cross-epoch counter state.
 *
 * Design decisions:
 *  - **Complete lines only.**  Bytes up to the last newline are parsed; a
 *    partial trailing line is buffered until its newline arrives, so a poll
 *    that races the producer's write cannot mis-parse a half-written line.
 *  - **Epoch completion = next E/H line.**  The wire format has no per-epoch
 *    terminator, so an epoch block is known complete only when the next epoch's
 *    E line (or a preceding RTA-10 H heartbeat, which always begins a new epoch
 *    unit) arrives.  A live consumer therefore sees each epoch when the producer
 *    flushes the following one: one product interval of latency.  Emitting at
 *    end-of-data instead would corrupt any epoch whose block straddles an
 *    ofstream buffer flush.  The producer must run
 *    @c network.product_flush_per_epoch for this latency to be bounded at all.
 *  - **Mid-file join** (@p start_at_end, or a consumer restart against a large
 *    file): (i) the contract is picked up from the next S line, so arm
 *    @c network.contract_rebroadcast_s on the producer, or a late joiner has no
 *    contract until the producer restarts; (ii) discontinuity-counter values are
 *    adopted from the wire when present (@c network.serialize_disc_counters)
 *    through replay's adoption path instead of a replay from the start; (iii) the
 *    RTA-3 generation id surfaces per epoch (@c NetworkSolutionEpoch::generation_id)
 *    and on the contract (@c SbasisContract::generation_id).
 *  - **Truncation / rotation.**  A file-size shrink means the producer
 *    restarted (the writer truncates on open): the tail warns, reopens from the
 *    start and resets its parse/emitter state.  A rewrite to the same or a
 *    larger size cannot be told apart from growth (a plain path has no inode
 *    identity); the tail resyncs at the next complete line and recovers its
 *    contract at the next S line.
 *
 * The owned replay emitter is constructed via
 * @c NetworkProductReader::makeReplayEmitter when the first S contract is seen,
 * provided no epoch has been replayed yet (the normal from-the-start case).  A
 * mid-file joiner that replays epochs before its first S line keeps the
 * default-constructed emitter and relies on RTA-4 counter adoption.
 */
class NetworkProductTail {
public:
    /**
     * @brief Attach to @p path.  The file need not exist yet; polls return
     *        nothing until it appears.
     * @param path         Product file being appended to by the producer.
     * @param start_at_end Join at the current end of file (mid-file join)
     *                     instead of parsing from the beginning.  Default false:
     *                     a consumer start replays the whole file so far, which
     *                     reconstructs every discontinuity counter exactly even
     *                     without RTA-4 serialized values.
     */
    explicit NetworkProductTail(const std::string& path, bool start_at_end = false);
    ~NetworkProductTail();
    NetworkProductTail(const NetworkProductTail&)            = delete;
    NetworkProductTail& operator=(const NetworkProductTail&) = delete;

    /// Path the tail was constructed with (for diagnostics).
    const std::string& path() const;

    /// Current S-basis contract state: the v2 header S line or the most recent
    /// RTA-5 re-broadcast consumed so far.  @c present is false until any S line
    /// has been seen (v1 files, or a mid-file join before the next re-broadcast).
    const SbasisContract& contract() const;

    /// The most recent epoch-status heartbeat (H record, RTA-10) consumed so far,
    /// or @c std::nullopt if none (pre-RTA-10 files, heartbeat off, or a mid-file
    /// join before the next H).  Updates on every consumed H line, including the
    /// standalone heartbeat of a fully withheld epoch, which completes no product
    /// record, so a live consumer can read the producer's last status and its time
    /// whether or not products flowed.  Reset by a rotation reopen.
    std::optional<NetworkProducerStatus> lastStatus() const;

    /// True while every consumed line has parsed cleanly (the incremental
    /// equivalent of @c NetworkProductReader::read's @c ok flag; sticky, reset
    /// on a rotation reopen).  A mid-file join's first torn line does not count:
    /// it is discarded before parsing.
    bool healthy() const;

    /// Number of epochs replayed into a store via @ref pollInto since
    /// construction or the last rotation reopen.
    std::size_t epochsDelivered() const;

    /**
     * @brief Parse-only poll: consume newly appended complete lines and return
     *        the epochs completed since the last poll (each exactly once, in
     *        file order).  Does not touch any store or the owned emitter.
     */
    std::vector<NetworkProductRecord> poll();

    /**
     * @brief Poll and replay each newly completed epoch into @p store via the
     *        owned replay emitter (@c NetworkProductReader::replay, shared
     *        logic, RTA-4 counter adoption included).
     * @return The completed records, so the caller can inspect per-epoch
     *         metadata (contract refreshes, generation id) without re-parsing.
     */
    std::vector<NetworkProductRecord> pollInto(SsrCorrectionStore& store);

private:
    struct Impl;                    ///< Parse/file state (defined in the .cpp).
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The a-priori orbit/clock identity of the S contract, naming the SP3 and CLK
 *        files in force during the run's processing window (card OWD, O1).
 *
 * The path resolver merges the previous, the observation and the next day's products,
 * so the first SP3 in the merged list is the previous day's file and cannot serve as
 * the identity.  A product whose IGS long name carries its span
 * (@c _YYYYDDDHHMM_NNU_, U in M/H/D/W/Y, the start on the GPS time scale) is in force
 * when that half-open span meets the half-open window [@p t0_gps_s, @p t1_gps_s).  The
 * identity lists the SP3 files in force, then the CLK files in force, each in resolver
 * order, joined with '+' (one day: "<sp3>+<clk>", the pre-OWD shape).  When a name of a
 * kind does not parse, or no file of a kind meets the window, every merged file of that
 * kind is named instead and @p named_all is set.  An absent kind reads '?'.
 *
 * @param product_paths The resolved product paths (@c GnssConfig::getProductPaths()).
 * @param t0_gps_s      Window start, continuous GPS seconds.
 * @param t1_gps_s      Window end (exclusive), continuous GPS seconds.
 * @param named_all     Optional out: true when a kind fell back to all its files.
 * @return The identity, or an empty string when there is no SP3 and no CLK file (the
 *         caller keeps its configuration-intent identity).
 */
std::string aprioriIdentityForWindow(const std::vector<std::filesystem::path>& product_paths,
                                     double t0_gps_s, double t1_gps_s,
                                     bool* named_all = nullptr);

/// Prefix of the S line's a priori identity under card NBP's native broadcast clock a
/// priori (@c network.apriori_source @c sp3_brdc_clock): "sp3+brdc:<SP3>|<nav>|ttom".
/// Under it every C record's clock refers to the stamped broadcast ephemeris (@c iode=),
/// so a consumer must hold that record and must not load a CLK product.
inline constexpr const char* kNbpAprioriPrefix = "sp3+brdc:";

/**
 * @brief Whether an S line's a priori identity declares card NBP's native broadcast clock.
 * @param apriori_product_id The S line's a priori identity.
 * @return True when it starts with @ref kNbpAprioriPrefix.
 */
inline bool isNbpAprioriId(const std::string& apriori_product_id) {
    return apriori_product_id.rfind(kNbpAprioriPrefix, 0) == 0;
}

} // namespace GnssProcessor
