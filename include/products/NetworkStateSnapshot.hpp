#pragma once
/**
 * @file NetworkStateSnapshot.hpp
 * @brief RTB-7: atomic periodic state snapshot + warm-resume gate for the
 *        PPP-RTK network producer.
 *
 * A long-lived network-producer service that crashes (or is bounced by a
 * supervisor) loses ALL of its cross-epoch state — the D2 shared carry, the
 * per-station ambiguity/ZWD carries, the two-state clock oscillators, the pivot
 * baselines, the pending retro-bump sets, the RTA-3 generation id, and the
 * emitter's discontinuity bookkeeping (counters, bias baselines, first-sighting
 * sets, RT0-5 pending forced-bump flags, RTA-11 last-seen stamps).  Restarting
 * cold forces every consumer through a global datum break and a fresh
 * convergence transient even when the outage was a few seconds.
 *
 * This module gives the producer a WARM path: @ref NetworkStateSnapshot is a
 * plain-data capture of BOTH state layers (driver + emitter) plus metadata, and
 * @ref NetworkStateSnapshotIO (de)serializes it to a versioned, line-oriented
 * text file (debuggable; doubles written with the shortest round-trippable
 * decimal via fmt's default formatting, exactly like @c NetworkProductIO) with
 * an ATOMIC replace on write — a crash mid-write can never corrupt the previous
 * good snapshot.
 *
 * Warm-vs-cold decision (@ref NetworkStateSnapshotIO::warmResumeAllowed): a
 * resume is WARM only when (1) the snapshot's config fingerprint matches the
 * live run's (resuming under a changed datum-relevant config must cold-start),
 * and (2) the snapshot's product-time age against the FIRST live epoch is
 * within the carry-validity horizon (the max of the per-key gap bounds,
 * @c network.phase_leveling_max_gap_s / @c network.clock_model_max_gap_s).  The
 * top-level gate is deliberately coarse: after a warm restore, PER-KEY
 * staleness is judged by the EXISTING downstream machinery — the local-carry
 * max-gap drop, the D2 @c q·dt inflation over the true gap (the restored
 * @c prev_week/prev_tow make @c dt span the outage), and the clock-model
 * per-satellite @c t_last reseed — so the snapshot restores everything and
 * lets the established per-key logic decide what is still usable.
 *
 * On a WARM resume the snapshot's RTA-3 generation id is PRESERVED (the
 * first-emit minting site is suppressed by the restored non-zero id), so
 * consumers see the SAME product datum and never hard-reset.  Anything else —
 * parse failure, fingerprint mismatch, stale age — cold-starts SAFELY: the
 * snapshot is ignored, a warning names the reason, and a NEW generation id is
 * minted at first emit as usual (consumers hard-reset, the RTB-7(b) quality
 * ramp degrades the URA until convergence).
 *
 * File format (line-oriented, one record per line, comma-separated; '#' only on
 * the first line):
 * @code
 *   # gnss_network_state_snapshot v1
 *   M,<week>,<tow>,<wall_utc>,<generation_id>,<fingerprint hex16>
 *   N,<n_emitted>,<n_skipped>,<n_gate_withheld>,<n_sigma0_withheld>,
 *     <n_runaway>,<n_perconst_partial>,<n_perconst_diffuse>,<iod>,
 *     <prev_week>,<prev_tow>,<last_emit_content_s>,<last_health_write_s>
 *   U,<ura_sigma0_ewma>,<ura_ewma_updates>,<ramp_lifted 0|1>,<first_emit_content_s>
 *   D,<kind>,<sat>,<signal>,<sh_n>,<sh_m>,<sh_trig>,<value>,<var>,<inflate_q>   # D2 shared carry
 *   V,<const>,<sat>                                     # prev_pivot
 *   W,<const>,<sat>                                     # last_pivot
 *   K,<sat>,<x0>,<x1>,<p00>,<p01>,<p11>,<t_last>,<init> # clock oscillator
 *   J,<sat>,<value>,<seen_s>                            # clock-monitor baseline
 *   L,<station_idx>,<sat>,<signal>,<value_abs>,<var>,<last_seen_s>  # ambiguity carry
 *   Z,<station_idx>,<value_abs>,<var>,<last_seen_s>     # ZWD carry
 *   R,<sat>                                             # pending retro-bump
 *   A,<sat>                                             # retro_active (edge-trigger)
 *   Y,<sat>,<epochs>                                    # URA refloat-inflation horizon
 *   C,<sat>                                             # c1_zoh_active (event edge-trigger)
 *   O,<sat>,x0,x1,p00,p01,p11,t_last,init               # emit_clock_rate decoupled C1 rate state
 *   I,<sat>,<iode>                                      # Card6 eph re-level baseline (brdc_rts)
 *   X,<sat>                                             # Card6 pending eph-cutover disc bump
 *   QS,<id>,<kind>,<sat>,<signal>,<sh_n>,<sh_m>,<sh_trig>,<sh_layer>,<s_prev>,<p_prev>
 *                                                       # Card1 coupling dictionary (v3)
 *   QL,<station_idx>,<sat>,<signal>,<k>,<id>,<M_rj>,...  # L row's regression, harvest order
 *   QZ,<station_idx>,<k>,<id>,<M_rj>,...                 # Z row's regression
 *   QR,<sat>                                             # write-epoch clock-refloat set
 *   EMITTER,v1,...                                      # emitter blob (verbatim,
 *   ...                                                 #  NetworkProductEmitter::serializeState)
 *   EEND
 *   END,<n_lines>                                       # truncation guard (line count)
 * @endcode
 * The @c END line counts every line after the '#' header (emitter blob lines
 * included); a missing or mismatching @c END fails the parse, which the caller
 * treats as the cold-start signal.  Station identity in the @c L / @c Z keys is
 * the ROSTER INDEX — valid only under an identical station roster, which the
 * config fingerprint guarantees.
 *
 * The header LITERAL carries the format version: @c v3 when any Card-1 coupling
 * record (@c QS / @c QL / @c QZ / @c QR) is present, @c v2 when only the §18.10
 * frame records are, @c v1 otherwise.  The reader accepts all three and an absent
 * record group simply restores empty containers, so an older file is always a
 * legal input and a run carrying neither group writes the v1 bytes unchanged.
 *
 * This module is entirely OUTSIDE the @c NetworkProductIO replay path: product
 * files know nothing of snapshots and replay reproduces every discontinuity
 * counter with no snapshot involved.
 */

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/**
 * @struct NetworkStateSnapshot
 * @brief Plain-data capture of the network producer's cross-epoch state
 *        (driver layer + serialized emitter layer) plus resume metadata.
 *
 * Pure data — the driver copies its loop-local state groups in/out; no field
 * references a driver-internal type, so the module is unit-testable without a
 * running network.
 */
struct NetworkStateSnapshot {
    // --- Metadata --------------------------------------------------------
    int         format_version{1};      ///< Snapshot format version (parse rejects others).
    int         snap_week{0};           ///< GPS week of the snapshotted (last processed) epoch.
    double      snap_tow{0.0};          ///< GPS time-of-week of the snapshotted epoch (s).
    std::string wall_utc;               ///< Wall-clock UTC write time (informational, ISO-8601).
    uint32_t    generation_id{0};       ///< RTA-3 solution-generation id (0 = never minted).
    uint64_t    config_fingerprint{0};  ///< FNV-1a-64 over the datum-relevant config fields.

    // --- Run counters (health/reporting continuity + the warmup gates) ----
    uint64_t n_emitted{0};           ///< Emitted-epoch count (drives @c carry_warm and the ramp).
    uint64_t n_skipped{0};           ///< Cumulative skipped epochs.
    uint64_t n_gate_withheld{0};     ///< Cumulative rank/null-space-gate withholds.
    uint64_t n_sigma0_withheld{0};   ///< Cumulative A4 σ̂0-ceiling withholds.
    uint64_t n_runaway_epochs{0};    ///< Cumulative RT0-5 runaway-quarantine epochs.
    uint64_t n_perconst_partial{0};  ///< Cumulative RTA-10 partial-publish epochs.
    uint64_t n_perconst_diffuse{0};  ///< Cumulative RTA-10 diffuse-publish epochs.
    int      iod{0};                 ///< Next IOD-SSR value (monotone across the bounce).
    int      prev_week{-1};          ///< Last processed epoch's GPS week (dt spans the outage).
    double   prev_tow{-1.0};         ///< Last processed epoch's TOW (s).
    double   last_emit_content_s{-1.0};   ///< Content time of the last emit (health continuity).
    double   last_health_write_s{-1.0};   ///< Content time of the last health row.

    // --- RTB-7(b) quality-ramp / URA EWMA state ---------------------------
    double   ura_sigma0_ewma{1.0};        ///< Network-wide σ̂0 EWMA scaling the emitted URA.
    uint64_t ura_ewma_updates{0};         ///< Number of σ̂0 EWMA updates received (ramp-lift gate).
    bool     ramp_lifted{false};          ///< Warmup URA floor already lifted (product grade reached).
    double   first_emit_content_s{-1.0};  ///< Content time of the first emit (grade-metric anchor).

    // --- D2 shared carry (driver @c prev_carry) ---------------------------
    /// One carried shared parameter — mirrors the driver's @c CarryPrior with
    /// @c SharedParamKind stored as its integer value (0 Clock, 1 CodeBias,
    /// 2 PhaseBias, 3 VtecSh).
    struct SharedCarry {
        int         kind{0};        ///< SharedParamKind as int.
        std::string sat;            ///< RINEX satellite id ("" for VtecSh).
        std::string signal;         ///< Signal ("" for Clock / VtecSh).
        int         sh_n{-1};       ///< VtecSh degree (VtecSh only).
        int         sh_m{-1};       ///< VtecSh order (VtecSh only).
        int         sh_trig{0};     ///< VtecSh 'C'/'S' as int (0 otherwise).
        double      value{0.0};     ///< Carried value.
        double      var{0.0};       ///< Carried posterior variance.
        bool        inflate_q{false}; ///< Clock-monitor soft-reinit mark.
    };
    std::vector<SharedCarry> shared_carry;  ///< In stash order (fold order preserved).

    std::map<std::string, std::string> prev_pivot;  ///< constellation -> pivot of the last emit.
    std::map<std::string, std::string> last_pivot;  ///< constellation -> last-EVER pivot (A1).

    /// Two-state clock-oscillator state — mirrors the driver's @c ClockOsc.
    struct Osc {
        double x0{0.0};    ///< Offset (m).
        double x1{0.0};    ///< Drift (m/s).
        double p00{0.0};   ///< cov(offset, offset).
        double p01{0.0};   ///< cov(offset, drift).
        double p11{0.0};   ///< cov(drift, drift).
        double t_last{0.0};///< Continuous-GPS s of the last update (gap logic).
        bool   init{false};///< Seeded at least once.
    };
    std::map<std::string, Osc> clock_osc;  ///< sat -> carried oscillator state.
    std::map<std::string, Osc> clock_rate_est;  ///< network.emit_clock_rate: decoupled C1 rate state (only x0/x1/t_last/init used).

    /// Clock-monitor adjacent-epoch baseline: sat -> {Δδtˢ (m), seen_s}.
    std::map<std::string, std::pair<double, double>> prev_clk;

    /// Per-station carried value — mirrors the driver's @c LocalCarry.
    struct Local {
        double value_abs{0.0};   ///< Carried absolute estimate (m).
        double var{0.0};         ///< Posterior conditional variance (m²).
        double last_seen_s{0.0}; ///< Continuous-GPS s when last stashed.
        double var_first{0.0};   ///< First-stash conditional variance (m²) — an OPTIONAL trailing L field written only when network.phase_datum_handover_max_var_ratio is armed (0 = absent; a pre-ratio file restores 0 and the ratio test refuses the arc until it re-converges).
    };
    /// (station roster index, sat, signal) -> ambiguity carry.  Index validity
    /// is guaranteed by the roster fields inside the config fingerprint.
    std::map<std::tuple<int, std::string, std::string>, Local> local_carry;
    std::map<int, Local> zwd_carry;  ///< station roster index -> ZWD carry (B9).

    std::map<std::string, int> ura_refloat_horizon; ///< sat -> post-refloat URA-inflation epochs left.
    std::set<std::string> ura_retro_sats;  ///< B1 retro-bumps pending for the next emit.
    std::set<std::string> retro_active;    ///< Currently-flagged sats (retro edge-trigger).
    std::set<std::string> c1_zoh_active;   ///< T5/R6 c1-ZOH event edge-trigger (log-only dedup).

    // --- Card 6 (RTF-2) ephemeris-cutover re-level state (brdc_rts only) ----
    /// sat -> broadcast IODE the carried clock state is currently referenced to
    /// (the eph-cutover re-level baseline).  Empty in sp3 mode / a pre-Card-6
    /// snapshot ⇒ the first brdc_rts epoch re-seeds it, so absence is safe.
    std::map<std::string, int> relevel_iode;
    /// sats with a pending forced eph-cutover discontinuity (prior eph version
    /// evicted before Δ_eph could be computed), awaiting their next emit.
    std::set<std::string> eph_cutover_disc_pending;

    // --- Card 4 (M1) invariant DD carry (RTB-7 branch-faithful resume) ------
    /// The ArcKey vertex numbering the carry blob references (network.state_snapshot_carry):
    /// @c carry_sat_vertex maps satellite id → its stable (first-seen-order) vertex id and
    /// @c carry_group_id maps a (constellation, signal) to its stable group id.  Both are
    /// restored so the reconstructed chords reference the SAME satellites/groups next epoch
    /// (station vertices are the roster index — stable under the fingerprint — so no map is
    /// needed for them).  Empty unless the toggle armed (+carry_invariant); absence ⇒ the
    /// invariant carry cold-starts, the pre-feature behaviour.
    std::map<std::string, int> carry_sat_vertex;
    std::vector<std::tuple<char, std::string, int>> carry_group_id;  ///< (const, signal, group id).
    /// Verbatim @c NetworkInvariantCarryDriver::serializeCarry blob (its @c CARRY
    /// header through its @c CEND terminator, newline-separated) — the carried z-store
    /// (per-component DD means, dense @f$P^z@f$, tree/chord/vertex structure) = the branch.
    std::string carry_state;

    // --- Resume FRAME state (format v2; SPEC_synthetic_minimum_network §18.10) ----
    //
    // The realization-frame bookkeeping RTB-7 v1 deliberately omitted and the
    // §18.9 seam census showed to be the whole post-resume transient: without it
    // the first warm epoch's phase-anchor election runs INCUMBENT-LESS for every
    // (sat,signal) family, marks them all as re-referenced, erases every restored
    // leveling carry, marginalizes their D2 phase-bias prior and re-pins the frame
    // at fresh code seeds; the Q3 clock anchor folds LOOSE for its re-couple
    // horizon; and the CLK-R held frame restarts at zero.  Captured UNCONDITIONALLY
    // whenever a snapshot is written (the Z-row precedent) — a run that carries
    // none of it writes the v1 format byte-for-byte.  A reader that finds them
    // restores them into maps nothing reads unless `network.resume_frame_restore`
    // is armed (the forward-only resume response policy, not fingerprinted).
    /// "sat_signal" ambiguity key -> sticky phase-anchor STATION NAME (the
    /// incumbent the next election must find to keep the family's frame).
    std::map<std::string, std::string> anchor_incumbent;
    /// "sat_signal" -> R2 sticky continuous pin target (m): the absolute value the
    /// incumbent anchor is realized at.  Absent ⇒ the 0-frame (a cold pin realizes
    /// the same frame as long as the incumbent is kept).
    std::map<std::string, double> anchor_pin_target;
    /// Card Q3 guarded temporal clock anchor — per-satellite state (mirrors the
    /// driver's @c ClockAnchorSat).  Restoring it lets the first post-resume fold
    /// run ANCHORED over the true elapsed time instead of LOOSE from a first
    /// sighting; the stale test still runs on @c t_last (a resume older than the
    /// anchor's own max gap decouples honestly).
    struct ClockAnchorSat {
        double t_last{0.0};        ///< continuous-GPS s of the last fold.
        double last_value{0.0};    ///< prior mean folded at that epoch (m).
        double last_var_eff{0.0};  ///< prior variance folded at that epoch (m²).
        int    in_band{0};         ///< consecutive in-band epochs while LOOSE.
        bool   anchored{false};    ///< ANCHORED (true) or LOOSE.
    };
    std::map<std::string, ClockAnchorSat> clock_anchor;  ///< sat -> anchor state.
    /// CLK-R stage-1 held-frame engine state (@c NetworkClockEmitRebase): the
    /// per-constellation frame accumulator A_c, the per-sat previous held value +
    /// last-seen stamp, and the epoch count (so the F1 `resume_seam` datum mark —
    /// keyed on a fresh engine — does not fire when the frame actually continues).
    std::map<char, double> clk_frame_acc;                        ///< constellation -> A_c (m).
    std::map<std::string, std::pair<double, double>> clk_frame_prev; ///< sat -> {last held (m), last seen (s)}.
    long long clk_frame_epochs{0};                               ///< rebaseEpoch count.
    // --- Card 1 coupling regressors (format v3; SPEC_synthetic_minimum_network §18.12) ---
    //
    // The SECOND half of the resume frame restore.  Restoring a leveling carry's
    // (value, var) alone (v1/v2) injects it as a PLAIN DIAGONAL row at the PURE
    // CONDITIONAL variance — a combination NO continuous epoch produces: the legacy
    // diagonal carry is stiff at the MARGINAL variance, while the Card-1 row is stiff
    // at the conditional variance but SHARED-NEUTRAL (its @f$A_s = M_r@f$ cancels the
    // reduced-normals contribution).  Measured (§18.11): 2811 such rows out-vote the
    // 9.5 mm Q3 anchored clock priors and 33 of 52 anchors trip at the first fold.
    /// One shared-border COLUMN's identity plus the two quantities that depend on the
    /// column and not on the carried row.  A POD mirror of the driver's @c CoupledShared
    /// MINUS the coefficient, with @c SharedParamKind and the SH trig char stored as
    /// their integer values exactly as @ref SharedCarry does.
    ///
    /// @note @c p_prev == 0 is MEANINGFUL — a datum-pinned or ensemble-gauge column is
    ///       "exactly known" (Card 1e, src/core/app/NetworkRunContext.cpp:2609-2632).
    ///       A reader must never treat it as absent or re-derive it: the κ shrink is
    ///       guarded on @c p_prev > 0 (NetworkCoupling.cpp:277), so re-deriving would
    ///       shrink a column the datum re-applies exactly and change the row's gauge sum.
    /// @note @c sh_layer is its OWN field, never the @c D record's "L1"-in-the-sat-string
    ///       hack (NetworkRunContext.cpp:1631-1634): @c resolveSharedCol resolves through
    ///       @c vtecCoeffColumn(n,m,trig,sh_layer), so a lost layer would bind a layer-1
    ///       coefficient to the live layer-0 column.
    struct CouplingCol {
        int         kind{0};      ///< SharedParamKind as int: 0 Clock, 1 CodeBias, 2 PhaseBias, 3 VtecSh.
        std::string sat;          ///< satellite id (EMPTY for VtecSh).
        std::string signal;       ///< signal id (EMPTY for Clock and VtecSh).
        int         sh_n{0};      ///< SH degree (VtecSh).
        int         sh_m{0};      ///< SH order (VtecSh).
        int         sh_trig{'C'}; ///< SH cos/sin as the char's int value (67/83).
        int         sh_layer{0};  ///< SH shell index (VtecSh; 0 = legacy layer).
        double      s_prev{0.0};  ///< @f$\hat s_{\text{prev},j}=x_s(j)@f$ at harvest (m, datum-realized).
        double      p_prev{0.0};  ///< @f$P_{\text{prev},jj}@f$ at harvest (m², datum frame).  0 = pinned/exact.
    };
    /// Dense, first-encounter-ordered dictionary of every shared column any carried
    /// row's regression references.  Its index is the @c QL / @c QZ entry id.
    ///
    /// @note The dictionary KEY is (identity, @c s_prev, @c p_prev), NOT identity alone.
    ///       @c harvestCoupling assigns both values from the shared column @c j
    ///       (NetworkRunContext.cpp:2629-2630), so rows of ONE harvest epoch necessarily
    ///       agree — but a station whose harvest FAILED keeps an older coupling
    ///       (@c continue before the stash, :2715), so two rows can legitimately hold
    ///       different VINTAGES of one identity.  Measured on the branch-point file:
    ///       2799 of 2811 L rows at one stamp, 12 at three older ones.  An identity-only
    ///       hoist would silently restore those 12 against the wrong frame.
    std::vector<CouplingCol> coupling_dict;
    /// (station roster index, sat, signal) — the SAME key as @ref local_carry — mapped to
    /// the row's regression as (dictionary id, @f$M_{rj}@f$) pairs IN HARVEST ORDER.
    /// The order is preserved because it is OBSERVABLE: the row RHS and the Card-1f
    /// family deficit are accumulated in it (NetworkCoupling.cpp:294/:301, :75-113).
    std::map<std::tuple<int, std::string, std::string>,
             std::vector<std::pair<int, double>>> coupling_rows;
    /// station roster index -> the ZWD carry's regression (same shape).  ZWD coupling IS
    /// harvested (NetworkRunContext.cpp:3227) and IS injected
    /// (ApplicationNetworkAssembly.cpp:2956-2958), so the Z rows have the identical defect.
    std::map<int, std::vector<std::pair<int, double>>> coupling_zrows;
    /// Card 1d/1g: the write epoch's clock-monitor refloat set
    /// (@c NetworkCarryState::prev_clock_refloat_sats).  A hard-refloated satellite is
    /// SKIP-STASHED, so it has no @c D row and nothing else in this file can carry the
    /// flag; without it the first warm epoch's serial pre-pass
    /// (ApplicationNetworkAssembly.cpp:2320-2326) writes no @c tu_reinit_hist mark for it
    /// and every restored term on that satellite folds at FULL weight onto a re-drawn column.
    std::set<std::string> clock_refloat_sats;
    /// §18.12 (2a): the C1 dense D2 covariance side-car (@c SharedCarryCov) — identity keys
    /// in row order (@c QK records) and the lower triangle of P per row (@c QP records:
    /// row i holds P(i,0..i)).  The OTHER half of the coupling seam: with the coupling rows
    /// conditional from the first warm fold, the border they condition on must fold DENSE
    /// from the first fold too, or the measured 376-key diagonal asymmetry stays.  Written
    /// only under state_snapshot_coupling_capture on a shared_carry_full_cov posture;
    /// restored behind resume_coupling_restore.  Empty on every other file.
    std::vector<std::string> carry_cov_keys;
    std::vector<std::vector<double>> carry_cov_rows;
    /// True when any coupling record is present (the writer then emits the v3 header).
    ///
    /// @warning Deliberately SEPARATE from @ref hasFrameState.  That predicate ALSO gates
    ///          the resume-side frame restore (NetworkRunContext.cpp:1252), so widening it
    ///          would let a coupling-bearing snapshot with no PA/PT/CA rows claim frame
    ///          state it does not have — the restore would proceed with an empty
    ///          @c anchor_incumbent, the QUEUE-1(d) erase would fire anyway, and this card
    ///          would silently defeat itself.
    bool hasCouplingState() const {
        return !coupling_dict.empty() || !coupling_rows.empty()
            || !coupling_zrows.empty() || !clock_refloat_sats.empty()
            || !carry_cov_keys.empty();
    }
    /// True when any frame record is present (the writer then emits the v2 header).
    bool hasFrameState() const {
        return !anchor_incumbent.empty() || !anchor_pin_target.empty()
            || !clock_anchor.empty() || !clk_frame_acc.empty()
            || !clk_frame_prev.empty() || clk_frame_epochs > 0;
    }
    // --- Emitter layer -----------------------------------------------------
    /// Verbatim @c NetworkProductEmitter::serializeState blob (its own
    /// @c EMITTER header through its @c EEND terminator, newline-separated).
    /// Restored via @c NetworkProductEmitter::restoreState — counters, bias
    /// baselines, first-sighting sets, RT0-5 pending forced-bump flags and
    /// RTA-11 last-seen stamps all round-trip exactly.
    std::string emitter_state;
};

/**
 * @class NetworkStateSnapshotIO
 * @brief Serialization, atomic file replace, and the warm/cold resume gate for
 *        @ref NetworkStateSnapshot.
 */
class NetworkStateSnapshotIO {
public:
    /**
     * @brief Serialize @p snap to @p os in the versioned line format (see the
     *        file banner).  Deterministic: maps/sets emit in key order, the
     *        shared-carry vector in element order.
     * @param os   Destination stream (caller owns; no flush performed).
     * @param snap Snapshot to serialize.
     */
    static void serialize(std::ostream& os, const NetworkStateSnapshot& snap);

    /**
     * @brief Parse a snapshot from @p is.  Strict: an unknown record tag, a
     *        malformed field, a missing metadata line, a truncated emitter
     *        section, or a missing/mismatching @c END line-count terminator all
     *        FAIL the parse (the caller's cold-start signal).  @p out is
     *        written only on success.
     * @param is  Source stream.
     * @param out Parsed snapshot (untouched on failure).
     * @param err Optional human-readable failure reason.
     * @return True on a complete, well-formed parse.
     */
    static bool parse(std::istream& is, NetworkStateSnapshot& out, std::string* err = nullptr);

    /**
     * @brief Atomically write @p snap to @p path.
     *
     * Serializes to <tt>path + ".tmp"</tt>, closes it, then replaces @p path
     * via @c std::filesystem::rename.  On MSVC, @c rename maps to
     * @c MoveFileExW with @c MOVEFILE_REPLACE_EXISTING, so an existing target
     * is replaced atomically; should an implementation refuse an existing
     * target (POSIX semantics differ from some Windows layers), the fallback
     * removes the target and renames again.  A crash before the rename leaves
     * the previous good snapshot untouched (only the @c .tmp is partial); a
     * crash after it leaves the new complete snapshot.  The previous snapshot
     * is never opened or modified in place.
     *
     * @param path Destination snapshot path.
     * @param snap Snapshot to persist.
     * @param err  Optional failure reason (open/write/rename).
     * @return True when the replace completed.
     */
    static bool write(const std::filesystem::path& path, const NetworkStateSnapshot& snap,
                      std::string* err = nullptr);

    /**
     * @brief Read and parse a snapshot file.
     * @param path Snapshot path.
     * @param out  Parsed snapshot (untouched on failure).
     * @param err  Optional failure reason (open failure or any @ref parse error).
     * @return True on success.
     */
    static bool read(const std::filesystem::path& path, NetworkStateSnapshot& out,
                     std::string* err = nullptr);

    /**
     * @brief The RTB-7 warm/cold resume gate.
     *
     * WARM requires BOTH:
     *  1. @c snap.config_fingerprint == @p live_config_fingerprint — the
     *     datum-relevant config (station roster order, pivot pins, carry q's,
     *     clock-model parameters, emitter channel toggles) is unchanged; and
     *  2. the snapshot's product-time age against the first live epoch,
     *     <tt>first_live_epoch_s − (snap_week·604800 + snap_tow)</tt>, is
     *     strictly positive (resuming AT or BEFORE the snapshotted epoch would
     *     double-fold that epoch's carry) and at most @p carry_horizon_s (the
     *     max of the per-key gap bounds — past it every carried key would be
     *     dropped/reseeded downstream anyway, so a warm restore buys nothing
     *     and only risks a stale datum).
     *
     * Per-key staleness INSIDE the horizon is deliberately not judged here:
     * the existing downstream gap-inflation / max-gap-drop / t_last-reseed
     * machinery evaluates each restored key on the first resumed epoch.
     *
     * @param snap                    Parsed snapshot.
     * @param live_config_fingerprint The resuming run's fingerprint.
     * @param first_live_epoch_s      Continuous-GPS seconds of the first epoch
     *                                the resuming run will process.
     * @param carry_horizon_s         Carry-validity horizon (s of product time).
     * @param reason                  Optional explanation (both outcomes).
     * @return True when a warm resume is allowed.
     */
    static bool warmResumeAllowed(const NetworkStateSnapshot& snap,
                                  uint64_t live_config_fingerprint,
                                  double first_live_epoch_s,
                                  double carry_horizon_s,
                                  std::string* reason = nullptr);

    /// FNV-1a-64 over @p s (the config-fingerprint hash primitive; same
    /// constants as the L15 manifest's config-bytes hash).
    static uint64_t fnv1a64(const std::string& s);

    /// Current wall-clock UTC as "YYYY-MM-DDTHH:MM:SSZ" (snapshot metadata).
    static std::string utcNowIso();
};

/**
 * @brief RTB-7(b): the startup quality-ramp LIFT rule (pure).
 *
 * The warmup URA floor (@c network.warmup_ura_floor_m) stays applied until the
 * producer is genuinely at product grade, i.e. BOTH:
 *  - @p n_emitted ≥ max(@p warmup_epochs, @p phase_leveling_warmup_epochs) —
 *    the carry population is warm (the phase-leveling warmup gates when carries
 *    start at all, so counting emitted epochs alone below it would lift while
 *    the solution is still carry-cold); and
 *  - the σ̂0 EWMA has received at least @p warmup_epochs updates
 *    (@p ewma_updates), OR the EWMA is structurally unavailable
 *    (@p ewma_structurally_available false — the RTB-6 sentinel condition,
 *    @c emit_ura && @c raim && @c phase_leveling not all armed; then there is
 *    no stabilization statistic to wait for and the emitted-epoch count alone
 *    decides).
 *
 * Counts are "through the previous epoch" (the producer evaluates before
 * emitting the current one), matching the pre-fix floor's indexing.
 *
 * @param n_emitted                    Epochs emitted before the current one.
 * @param warmup_epochs                @c network.warmup_epochs.
 * @param phase_leveling_warmup_epochs @c network.phase_leveling_warmup_epochs.
 * @param ewma_updates                 σ̂0 EWMA updates received so far.
 * @param ewma_structurally_available  Whether the EWMA can update at all this
 *                                     run (emit_ura && raim && phase_leveling).
 * @return True when the DEGRADED floor lifts (product grade reached).
 */
inline bool networkWarmupRampLifted(long long n_emitted, int warmup_epochs,
                                    int phase_leveling_warmup_epochs,
                                    long long ewma_updates,
                                    bool ewma_structurally_available) {
    const long long need_epochs =
        (warmup_epochs > phase_leveling_warmup_epochs) ? warmup_epochs
                                                       : phase_leveling_warmup_epochs;
    if (n_emitted < need_epochs) return false;
    if (!ewma_structurally_available) return true;
    return ewma_updates >= static_cast<long long>(warmup_epochs);
}

} // namespace GnssProcessor
