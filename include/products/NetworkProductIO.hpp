#pragma once
/**
 * @file NetworkProductIO.hpp
 * @brief On-disk (de)serialization of the PPP-RTK NETWORK joint solution.
 *
 * Sink A of the network product-emit path (@c NetworkProductEmitter) pushes each
 * epoch's estimated satellite-side solution into an in-memory
 * @c SsrCorrectionStore for the in-process closed-loop consumer.  This module
 * adds the *persistent* half: a writer that appends each @c NetworkSolutionEpoch
 * to a plain-text product file as the producer runs, and a reader that parses it
 * back so consumer-side experiments (notably the consumer AR-frame work) can
 * replay the products WITHOUT re-running the expensive multi-station network.
 *
 * What is serialized is the *pre-emit* @c NetworkSolutionEpoch (estimable
 * satellite clock + code/phase bias) plus the per-epoch @c datum_changed flag —
 * the one piece of stream state the epoch struct itself does not carry (it lives
 * in the driver, set on a clock-pivot handover).  The discontinuity counters are
 * by default NOT serialized: they are stream state owned by
 * @c NetworkProductEmitter and are recomputed deterministically on replay.
 * Feeding the parsed records back through the SAME
 * @c NetworkProductEmitter::emitToStore (via @c replay, which also re-applies
 * @c markDatumChange) therefore reproduces the producer's live
 * @c SsrCorrectionStore contents byte-for-byte, discontinuity counters included.
 * RTA-4 (@c network.serialize_disc_counters) additionally writes the post-emit
 * counter VALUES as optional trailing fields so a late-joining / tail reader can
 * reconstruct them without replaying from epoch 1; @c replay keeps the recompute
 * as a self-check and adopts the file values on disagreement.
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
 * Optional records/fields (each present ONLY when its producer toggle is on, so
 * an OFF run writes the base format verbatim):
 * @code
 *   I,<generation_id>                       # RTA-3 solution-generation id (after E/G)
 *   C,<sat>,<c0>[,<c1>][,<clock_disc>]      # RTA-4: with counters on, c1 is ALWAYS
 *     [,iode=<n>][,oiod=<n>]                #   written (0 when absent) so the clock
 *     [,oepoch=<week>:<tow>]                #   disc counter sits at fixed field 5.
 *                                           #   Card 4 (RTF-2): the iode=/oiod=/
 *                                           #   oepoch= key=value tokens carry the
 *                                           #   brdc_rts broadcast-IODE anchor + RTS
 *                                           #   orbit echo (all absent in sp3 mode;
 *                                           #   a field with '=' ends the positional
 *                                           #   c1/clock_disc parse)
 *   B,<sat>,<signal>,<bias>[,<code_disc>]   # RTA-4 code disc counter (field 5)
 *   P,<sat>,<sig>,<b>,<si>,<wl>[,<ph_disc>] # RTA-4 phase disc counter (field 7)
 *     [,sigma=<m>][,sigc=<m>]               # B1b phase-bias σ (sigma=) + M4 combined
 *                                           #   phase-correction σ (sigc=; clock+phasebias)
 *   C,...[,sigcc=<m>][,rho=<val>]           # M4 per-sat combined code-correction σ
 *                                           #   (sigcc=) + code/phase corr (rho=)
 *   S,...[,gid=<id>][,stale=<sec>]          # RTA-3/RTA-11 trailing key=value fields;
 *     [,frame=<id>][,cepoch=<epoch>]        #   RTA-12 contract identity fields
 *     [,agek=<val>]                         #   M4 combined-σ age-scaling coefficient
 *     [,atx=<file>][,oref=<CoM|APC>]        #   (each absent when empty);
 *                                           #   repeated S lines mid-file are RTA-5
 *                                           #   contract re-broadcasts (positional)
 *   H,<week>,<tow>,<status>,<sigma0>,       # RTA-10 epoch-status heartbeat
 *     <n_sats_published>,<withheld>         #   (network.emit_epoch_status_record)
 * @endcode
 * The @c H heartbeat (RTA-10) is POSITIONAL, like the S contract line: written
 * immediately BEFORE the epoch block it describes (status @c ok / @c partial),
 * or standing ALONE — no E line — for a fully withheld epoch (status
 * @c withheld_sigma0 / @c withheld_gate; @c withheld is "*" there, "-" on a
 * clean emit, else semicolon-joined constellation letters).  @c sigma0 is the
 * global within-epoch σ̂0 ("%.4f"; 0 when unavailable).  Like S it NEVER feeds
 * @c replay — the twin file without H lines reconstructs a byte-identical
 * store.  Format-version note: a PRE-RTA-10 reader treats @c H as an unknown
 * tag — it still parses every epoch correctly but flags its sticky @c ok /
 * @c healthy() as false; arm @c network.emit_epoch_status_record only when the
 * consumers reading the file are current.  (The '#' header legend is NOT
 * extended — the v2 legend bytes are frozen so an OFF run stays byte-identical.)
 * Floating-point fields are written with the shortest round-trippable decimal
 * (fmt's default formatting), so a parse recovers the exact @c double.  A
 * satellite with no @c C line round-trips to @c has_clock=false; a satellite with
 * no records at all is dropped (it would push nothing through @c emitToStore, so
 * the store result is identical).
 */

#include "products/NetworkProductEmitter.hpp"   // NetworkSolutionEpoch, NetworkProductEmitter
#include "products/SsrCorrectionStore.hpp"      // SsrCorrectionStore, NetworkProducerStatus (RTA-10)
#include "filter/NetworkSbasis.hpp"             // SbasisDeclaration (B6 contract)

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/// B6: the S-basis contract stamped into a v2 product header (the "S" line) so a
/// consumer can cross-check compatibility before claiming integer recovery.  Holds
/// the S-basis convention (@ref SbasisDeclaration), the a-priori orbit/clock product
/// identity the emitted clock C0 is a delta against, and the phase-bias
/// discontinuity threshold the emitter used.  @c present is true only when a v2
/// header actually carried it (a v1 file / OFF run leaves it default).
struct SbasisContract {
    SbasisDeclaration decl;                   ///< The realized S-basis convention.
    std::string       apriori_product_id;     ///< A-priori orbit/clock product identity, e.g. "CODFIN2024129".
    /// Phase-bias discontinuity threshold the emitter used.  The default is the
    /// emitter's own (R9: single constant) — it is what @c makeReplayEmitter applies
    /// to a v1 file with no S line, so it must track the live emitter default.
    double            disc_threshold_m{NetworkProductEmitter::kDefaultDiscThresholdM};
    /// H3: per-λ fractional-cycle discontinuity threshold
    /// (@c network.disc_threshold_frac_cycles).  When > 0 the emitter's phase (and
    /// opt-in code) jump test uses @c frac·λ(sat,signal) instead of the flat
    /// @c disc_threshold_m — the SIXTH replay-emitter construction parameter.
    /// Serialized as an optional trailing @c discfrac= S-line field ONLY when > 0,
    /// so a legacy (flat-metre) producer writes the S line byte-for-byte.
    double            disc_threshold_frac_cycles{0.0};
    /// Whether the producer's emitter maintained the per-satellite CLOCK
    /// discontinuity counter (A2).  Together with @c disc_threshold_m this fully
    /// determines the @c NetworkProductEmitter construction, so a consumer can
    /// replay the file with byte-identical discontinuity counters and no
    /// out-of-band knowledge of the producer's config (see @c makeReplayEmitter).
    bool              emit_clock_disc{false};
    /// RTA-11: the emitter's staleness horizon (s, product time; 0 = off) — the
    /// third replay-emitter construction parameter (see @c makeReplayEmitter).
    /// Serialized as an optional trailing @c stale= field on the S line ONLY when
    /// non-zero, so an OFF run's v2 header is byte-identical to pre-RTA-11.
    double            staleness_horizon_s{0.0};
    /// M4 (contract v2): age-scaling coefficient the consumer uses to inflate the
    /// combined-correction variance as σc²·(1 + agek·Δt_age²) (Δt_age = correction
    /// content age, s), replacing the ad-hoc URA staleness ramp for network-corrected
    /// rows.  From the producer's @c network.combined_sigma_agek.  Serialized as an
    /// optional trailing @c agek= S-line field ONLY when non-zero, so a producer that
    /// does not declare it writes a byte-identical S line.  A quality hint, not a
    /// replay-emitter parameter — @c makeReplayEmitter ignores it and @c checkContract
    /// does not compare it.
    double            age_scale_k{0.0};
    /// Whether the producer's emitter ran the opt-in CODE-bias magnitude jump
    /// test (@c network.code_bias_disc_jump_test) — the fourth replay-emitter
    /// construction parameter.  Serialized as an optional trailing @c cbjump=1
    /// field ONLY when true (byte-identical OFF).
    bool              code_bias_jump_test{false};
    /// Whether the producer's emitter bumped clock disc counters on a
    /// constellation clock-set change (@c network.clock_disc_on_set_change, the
    /// sum-to-zero datum-move signal) — the fifth replay-emitter construction
    /// parameter.  Serialized as an optional trailing @c clkset=1 field ONLY
    /// when true (byte-identical OFF).
    bool              clock_disc_on_set_change{false};
    /// RTA-3: the producer's solution-generation id (0 = not stamped).  Serialized
    /// as an optional trailing @c gid= field on the S line ONLY when non-zero, so
    /// an OFF run's v2 header is byte-identical.  Lets a late joiner recover the
    /// generation from the (re-broadcast, RTA-5) contract line alone.
    uint32_t          generation_id{0};
    // --- RTA-12 contract identity fields --------------------------------------
    // Each serializes as an optional trailing key=value S-line field ONLY when
    // non-empty (following the gid=/stale= pattern), so a producer that declares
    // none of them writes a byte-identical S line.  checkContract compares each
    // ONLY when the consumer expectation declares it non-empty (the
    // apriori_product_id semantics).
    /// Reference frame the fixed station coordinates are expressed in, e.g.
    /// "IGS20" (config declaration @c network.frame_id; @c frame= field).
    std::string       frame_id;
    /// Epoch the station coordinates are valid at — a decimal year ("2024.5") or
    /// an ISO date ("2024-07-01").  From @c network.coord_epoch, or defaulted to
    /// the SIF propagation date under @c network.use_sif_coordinates
    /// (@c cepoch= field).
    std::string       coord_epoch;
    /// Basename of the antenna-calibration (ATX) file the producer run actually
    /// loaded, e.g. "igs20.atx" — empty when unresolvable (@c atx= field).
    std::string       atx_id;
    /// Orbit reference point of the a-priori orbit/clock products: "CoM"
    /// (center of mass; the SP3+ATX convention) or "APC" (antenna phase center;
    /// SSR orbit streams).  @c oref= field.
    std::string       orbit_ref;
    bool              present{false};         ///< True when parsed from (or written to) a v2 header.
};

/// One serialized network-product record: the epoch's solution plus the
/// driver-side @c datum_changed flag (a clock-pivot handover this epoch), which
/// @c NetworkProductReader::replay turns back into a forced discontinuity bump.
struct NetworkProductRecord {
    NetworkSolutionEpoch epoch;                 ///< The estimable satellite-side solution.
    bool                 datum_changed{false};  ///< Clock-pivot / S-basis re-declaration this epoch.
    /// Constellations whose clock datum re-referenced this epoch (A2 per-
    /// constellation scoping).  Serialized as E-line field 8 ONLY when non-empty;
    /// an empty set (old-format files, or a run with per-constellation scoping off)
    /// makes @c replay force a GLOBAL bump — reproducing the pre-A2 behavior.
    std::set<char>       datum_consts;
    /// Satellites the B1 retro-bump flagged for a per-satellite discontinuity this
    /// epoch (a post-emit QC tier caught contamination the previous epoch).
    /// Serialized as a separate @c R record (comma-separated sat ids) ONLY when
    /// non-empty, so a run with the retro-bump off writes the pre-B1 format
    /// verbatim; @c replay turns it back into @c markSatDiscontinuity.
    std::set<std::string> retro_sats;
    /// RTA-4: discontinuity-counter VALUES parsed from the optional trailing
    /// fields (C field 5 / B field 5 / P field 7; present only when the producer
    /// ran with @c network.serialize_disc_counters).  They are the POST-EMIT
    /// counters of THIS epoch — exactly what the producer's @c emitToStore pushed
    /// into its live store.  @c replay keeps its deterministic recompute and uses
    /// these as a self-check: on any disagreement (the expected case for a reader
    /// that joined mid-file) it warns, ADOPTS the values into the replay emitter
    /// and re-stamps the store records.  Empty maps for old files / OFF runs.
    struct SerializedDiscCounters {
        std::map<std::string, int> clock;  ///< "sat"        -> clock disc counter (C field 5).
        std::map<std::string, int> code;   ///< "sat|signal" -> code disc counter  (B field 5).
        std::map<std::string, int> phase;  ///< "sat|signal" -> phase disc counter (P field 7).
        bool any() const { return !clock.empty() || !code.empty() || !phase.empty(); }
    };
    SerializedDiscCounters serialized_disc;
    /// RTA-5: the S-contract state as of THIS epoch — set (present=true) when an
    /// S line immediately preceded this epoch's E line (the deferred v2 header S
    /// counts for the first epoch; a mid-file re-broadcast counts for the epoch it
    /// precedes).  A consumer that declares an expectation should run
    /// @c checkContract on EVERY present refresh, not once at end of file.  S
    /// lines never feed @c replay, so replay determinism is unaffected.
    SbasisContract contract_refresh;
    /// RTA-10: this epoch's status heartbeat — set when an H line with MATCHING
    /// week/tow immediately preceded this epoch's E line (the producer writes the
    /// emitted epoch's H right before its block).  A standalone H (fully withheld
    /// epoch) attaches to NO record — it surfaces through
    /// @c NetworkProductTail::lastStatus instead.  H lines never feed @c replay.
    std::optional<NetworkProducerStatus> epoch_status;
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

    /// B6: open @p path and emit a v2 header carrying @p contract (S-basis
    /// declaration + a-priori product identity + disc threshold) before any epoch.
    /// The per-epoch E/C/B/P records are unchanged; only the header differs from v1.
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
    /// each C/B/P record gains an optional trailing field carrying the CURRENT
    /// (post-emit) discontinuity counter queried from this emitter — the caller
    /// must pass the SAME emitter whose @c emitToStore just processed @p sol, so
    /// the serialized values are identical to what the store received for this
    /// epoch.  On the C line the optional C1 drift field is then ALWAYS written
    /// (0 when absent) so the clock counter sits at a fixed position.  nullptr
    /// (default) writes the pre-RTA-4 field counts verbatim.
    /// No-op if the stream is not @c good().
    void write(const NetworkSolutionEpoch& sol, bool datum_changed = false,
               const std::set<char>& datum_consts = {},
               const std::set<std::string>& retro_sats = {},
               const NetworkProductEmitter* disc_source = nullptr);

    /// RTA-5: append a CURRENT machine-parseable "S" contract line mid-file (the
    /// periodic / datum-change contract re-broadcast).  Same grammar as the header
    /// S line, including the optional @c gid= / @c stale= trailing fields.  The
    /// caller writes it BEFORE the epoch block it describes, so a positional
    /// reader updates its contract state for the epochs that follow.  No-op if
    /// the stream is not @c good().
    void writeContract(const SbasisContract& contract);

    /// RTA-10: append one positional epoch-status heartbeat line
    /// (@c H,week,tow,status,sigma0,n_sats_published,withheld — see the file
    /// banner).  The producer driver writes it BEFORE the epoch block on an emit
    /// (S-line precedent) and STANDALONE at the gate / σ̂0 withhold sites, which
    /// otherwise emit nothing, so a live tail can distinguish "withheld" from
    /// "producer died".  An empty @p status.withheld is written as "-".  The
    /// caller owns the per-epoch flush (@c network.product_flush_per_epoch), as
    /// for epoch blocks.  No-op if the stream is not @c good().
    void writeEpochStatus(const NetworkProducerStatus& status);

    /// RTA-6: flush the output stream to disk.  The producer driver calls this
    /// once per epoch under @c network.product_flush_per_epoch so a LIVE tail
    /// consumer (@ref NetworkProductTail) sees each epoch block as soon as it is
    /// written rather than at ofstream-buffer boundaries (an epoch block larger
    /// than the stream buffer would otherwise reach disk in arbitrary partial
    /// chunks, and a smaller one possibly minutes late).  Flushing changes NO
    /// bytes — the file is identical with or without it — but a live tail
    /// REQUIRES it for bounded product latency.  No-op if the stream is not
    /// @c good().
    void flush() { if (out_) out_.flush(); }

private:
    /// Emit the format banner + legend; when @p contract is non-null, a v2 banner
    /// plus the machine-parseable "S" contract line (B6).  Called by both ctors.
    void writeHeader_(const SbasisContract* contract);

    /// Serialize one "S" contract line (shared by the v2 header and the RTA-5
    /// mid-file re-broadcast so the two grammars can never diverge).
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

    /// B6 overload: also surface the parsed S-basis contract (the v2 "S" header
    /// line).  @p contract_out->present is false for a v1 file / absent "S" line
    /// (backward-compatible: @p ok stays true and replay is unaffected).
    static std::vector<NetworkProductRecord> read(const std::string& path,
                                                  SbasisContract* contract_out,
                                                  bool* ok);

    /// B6: cross-check a file's S-basis contract against the consumer's @p expected
    /// convention.  Returns true if compatible; on any mismatch returns false and
    /// (if @p mismatches non-null) appends a human-readable line per differing field.
    /// The caller decides whether a mismatch is a warning or a hard error.
    static bool checkContract(const SbasisContract& file, const SbasisContract& expected,
                              std::vector<std::string>* mismatches = nullptr);

    /// L14: replace every ',' with ';' so an arbitrary string is safe to write into a
    /// single CSV field of @c network_contract_check.csv.  Defensive hardening — the
    /// current checkContract mismatch strings are comma-free, but this keeps the check
    /// output robust to future message edits.
    static std::string csvSanitize(const std::string& s);

    /// Construct an emitter configured exactly as the producer that wrote
    /// @p contract (phase-bias discontinuity threshold + clock-disc channel), so a
    /// replay reproduces EVERY discontinuity counter — including the clock channel —
    /// byte-for-byte with no out-of-band knowledge of the producer's config.  A v1
    /// file (@c contract.present false) yields the default emitter, reproducing the
    /// pre-B6 replay behavior.  Feed the SAME instance to every @c replay call of
    /// one file (the emitter carries the cross-epoch counter state).
    static NetworkProductEmitter makeReplayEmitter(const SbasisContract& contract);

    /**
     * @brief Replay one record into @p store via @p emitter, reproducing the live
     *        producer path: a @c markDatumChange (when @c rec.datum_changed) then
     *        @c emitToStore.  The consumer interleaves this with its own per-epoch
     *        processing exactly as @c runNetwork does live, so the store always
     *        reflects the just-replayed epoch.
     */
    static void replay(const NetworkProductRecord& rec,
                       SsrCorrectionStore& store,
                       NetworkProductEmitter& emitter);
};

/**
 * @class NetworkProductTail
 * @brief Incremental ("tail -f") reader for a GROWING network product file (RTA-6).
 *
 * A standalone real-time consumer follows the producer's product file while the
 * producer is still appending to it.  Each @ref poll consumes whatever complete
 * NEW lines have appeared since the last poll and returns the newly COMPLETED
 * epochs, exactly once each; @ref pollInto additionally replays every completed
 * epoch into a caller-supplied @c SsrCorrectionStore through the SAME
 * @c NetworkProductReader::replay path the batch reader uses (one shared code
 * path — including the RTA-4 serialized-counter adoption), driven by an OWNED
 * replay emitter that carries the cross-epoch counter state.
 *
 * Design decisions:
 *  - **Complete lines only.**  Bytes up to the last newline are parsed; a
 *    partial trailing line is buffered until its newline arrives, so a poll
 *    that races the producer's write can never mis-parse a half-written line.
 *  - **Epoch completion = next E/H line.**  The wire format has no per-epoch
 *    terminator, so an epoch block is known complete only when the NEXT epoch's
 *    E line (or a preceding RTA-10 H heartbeat, which always begins a new epoch
 *    unit) arrives.  A live consumer
 *    therefore sees each epoch when the producer flushes the FOLLOWING one —
 *    one product interval of latency.  Emitting at end-of-data instead would
 *    corrupt any epoch whose block straddles an ofstream buffer flush.  The
 *    producer must run @c network.product_flush_per_epoch for this latency to
 *    be bounded at all.
 *  - **Mid-file join** (@p start_at_end, or a consumer restart against a large
 *    file): (i) the contract is picked up from the NEXT S line — arm
 *    @c network.contract_rebroadcast_s on the producer so a late joiner is not
 *    blind until a restart; (ii) discontinuity-counter VALUES are adopted from
 *    the wire when present (@c network.serialize_disc_counters) via replay's
 *    existing adoption path instead of a replay-from-start; (iii) the RTA-3
 *    generation id surfaces per epoch (@c NetworkSolutionEpoch::generation_id)
 *    and on the contract (@c SbasisContract::generation_id).
 *  - **Truncation / rotation.**  A file-size SHRINK means the producer
 *    restarted (the writer truncates on open): the tail warns, reopens from the
 *    start and resets its parse/emitter state.  A same-size-or-larger rewrite
 *    is indistinguishable from growth (there is no inode identity on a plain
 *    path) — the tail resyncs at the next complete line and heals its contract
 *    at the next S line.
 *
 * The owned replay emitter is constructed via
 * @c NetworkProductReader::makeReplayEmitter the moment the first S contract is
 * seen (provided no epoch has been replayed yet — the normal from-the-start
 * case); a mid-file joiner that replays epochs before its first S line keeps
 * the default-constructed emitter and relies on RTA-4 counter adoption.
 */
class NetworkProductTail {
public:
    /**
     * @brief Attach to @p path.  The file need not exist yet — polls quietly
     *        return nothing until it appears.
     * @param path         Product file being appended to by the producer.
     * @param start_at_end Join at the CURRENT end of file (mid-file join)
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

    /// Current S-basis contract state — the v2 header S line or the most recent
    /// RTA-5 re-broadcast consumed so far.  @c present is false until any S line
    /// has been seen (v1 files, or a mid-file join before the next re-broadcast).
    const SbasisContract& contract() const;

    /// RTA-10: the most recent epoch-status heartbeat (H record) consumed so far,
    /// or @c std::nullopt if none (pre-RTA-10 files / heartbeat off / a mid-file
    /// join before the next H).  Updates on EVERY consumed H line — including the
    /// standalone heartbeat of a fully WITHHELD epoch, which completes no product
    /// record — so a live consumer can read the producer's last status and its
    /// time regardless of whether products flowed.  Reset by a rotation reopen.
    std::optional<NetworkProducerStatus> lastStatus() const;

    /// True while every consumed line has parsed cleanly (the incremental
    /// equivalent of @c NetworkProductReader::read's @c ok flag; sticky, reset
    /// on a rotation reopen).  A mid-file join's first torn line is NOT counted
    /// — it is discarded before parsing, not parsed.
    bool healthy() const;

    /// Number of epochs replayed into a store via @ref pollInto since
    /// construction or the last rotation reopen.
    std::size_t epochsDelivered() const;

    /**
     * @brief Parse-only poll: consume newly appended complete lines and return
     *        the epochs COMPLETED since the last poll (each exactly once, in
     *        file order).  Does not touch any store or the owned emitter.
     */
    std::vector<NetworkProductRecord> poll();

    /**
     * @brief Poll and replay each newly completed epoch into @p store via the
     *        owned replay emitter (@c NetworkProductReader::replay — shared
     *        logic, RTA-4 counter adoption included).
     * @return The completed records, so the caller can inspect per-epoch
     *         metadata (contract refreshes, generation id) without re-parsing.
     */
    std::vector<NetworkProductRecord> pollInto(SsrCorrectionStore& store);

private:
    struct Impl;                    ///< Parse/file state (defined in the .cpp).
    std::unique_ptr<Impl> impl_;
};

} // namespace GnssProcessor
