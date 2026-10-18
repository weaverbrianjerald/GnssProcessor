#pragma once
/**
 * @file NetworkStateSnapshot.hpp
 * @brief Atomic periodic state snapshot and warm-resume gate for the PPP-RTK
 *        network producer (RTB-7).
 *
 * A long-lived network-producer service that crashes, or is restarted by a
 * supervisor, loses all of its cross-epoch state: the D2 shared carry, the
 * per-station ambiguity and ZWD carries, the two-state clock oscillators, the pivot
 * baselines, the pending retro-bump sets, the RTA-3 generation id, and the
 * emitter's discontinuity bookkeeping (counters, bias baselines, first-sighting
 * sets, RT0-5 pending forced-bump flags, RTA-11 last-seen stamps).  A cold restart
 * forces every consumer through a global datum break and a fresh convergence
 * transient, even after an outage of a few seconds.
 *
 * This module provides the warm path.  @ref NetworkStateSnapshot is a plain-data
 * capture of both state layers (driver and emitter) plus metadata, and
 * @ref NetworkStateSnapshotIO serializes it to, and parses it from, a versioned,
 * line-oriented text file that can be read for debugging.  Doubles are written in
 * the shortest round-trippable decimal form (fmt's default formatting, as in
 * @c NetworkProductIO).  The file is replaced atomically on write, so a crash during
 * a write leaves the previous good snapshot intact (the one non-atomic fallback is
 * described at @ref NetworkStateSnapshotIO::write).
 *
 * Warm-vs-cold decision (@ref NetworkStateSnapshotIO::warmResumeAllowed): a resume is
 * warm only when (1) the snapshot's config fingerprint matches the live run's (a
 * resume under a changed datum-relevant config must cold-start), and (2) the
 * snapshot's product-time age at the first live epoch is within the carry-validity
 * horizon (the max of the per-key gap bounds, @c network.phase_leveling_max_gap_s /
 * @c network.clock_model_max_gap_s).  This top-level gate is coarse on purpose.
 * After a warm restore, per-key staleness is judged by the existing downstream
 * logic: the local-carry max-gap drop, the D2 @c q·dt inflation over the true gap
 * (the restored @c prev_week / @c prev_tow make @c dt span the outage), and the
 * clock-model per-satellite @c t_last reseed.  The snapshot restores everything, and
 * that per-key logic decides what is still usable.
 *
 * On a warm resume the snapshot's RTA-3 generation id is kept (the restored non-zero
 * id suppresses the first-emit minting site), so consumers see the same product datum
 * and do not hard-reset.  Every other outcome (parse failure, fingerprint mismatch,
 * stale age) cold-starts: the snapshot is ignored, a warning names the reason, and a
 * new generation id is minted at the first emit as usual (consumers hard-reset, and
 * the RTB-7(b) quality ramp degrades the URA until convergence).
 *
 * File format: line-oriented, one record per line, comma-separated, with '#' only on
 * the first line.  Records are listed in the order serialize() writes them.  parse()
 * does not depend on the order of the record groups (a @c QL row may precede its
 * @c L row), but it keeps the file order of the @c D, @c GG and @c CN rows, requires
 * the @c QS, @c QK and @c QP ids to be dense and ascending, and stops reading at @c END.
 * @code
 *   # gnss_network_state_snapshot v1                  # header literal (v2 / v3: see below)
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
 *   L,<station_idx>,<sat>,<signal>,<value_abs>,<var>,<last_seen_s>[,<var_first>]
 *                                                       # ambiguity carry (var_first optional)
 *   Z,<station_idx>,<value_abs>,<var>,<last_seen_s>     # ZWD carry
 *   PA,<sat>_<signal>,<station>                         # v2 frame: phase-anchor incumbent
 *   PT,<sat>_<signal>,<target>                          # v2 frame: sticky pin target
 *   CA,<sat>,<t_last>,<last_value>,<last_var_eff>,<in_band>,<anchored 0|1>
 *                                                       # v2 frame: Q3 clock anchor
 *   FA,<const>,<A_c>                                    # v2 frame: CLK-R frame accumulator
 *   FP,<sat>,<last_held>,<last_seen_s>                  # v2 frame: CLK-R per-sat held value
 *   FE,<epochs>                                         # v2 frame: CLK-R epoch count (if > 0)
 *   QS,<id>,<kind>,<sat>,<signal>,<sh_n>,<sh_m>,<sh_trig>,<sh_layer>,<s_prev>,<p_prev>
 *                                                       # coupling dictionary (v3)
 *   QL,<station_idx>,<sat>,<signal>,<k>,<id>,<M_rj>,...  # L row's regression, harvest order
 *   QZ,<station_idx>,<k>,<id>,<M_rj>,...                 # Z row's regression
 *   QR,<sat>                                             # write-epoch clock-refloat set
 *   QK,<i>,<key>                                         # dense D2 cov: key of row i
 *   QP,<i>,<i+1>,<P(i,0)>,...,<P(i,i)>                   # dense D2 cov: row i, lower triangle
 *   EQD,<cons>|<signal>,<station>                        # RS-3: eq34 sticky datum receiver
 *   EQP,<station>|<cons>|<signal>,<sat>                  # RS-3: receiver parent-edge sat
 *   EQS,<sat>_<signal>,<station>                         # RS-3: sat's component datum rx
 *   EQT,<station_idx>,<sat>_<signal>,<target>            # RS-3: last epoch's pin target
 *   EQE,<station_idx>,<sat>,<elev_deg>                   # RS-3: longevity elevation
 *   EQR,<station_idx>,<sat>_<signal>                     # RS-3: gap-retained tree edge
 *   CBP,<cons>|<signal>,<sat>,<value>                    # RS-3: code-bias prev value
 *   CBS,<sat>                                            # RS-3: code-bias ever-seen
 *   CBI,<sat>,<sig1>,<sig2>                              # RS-3: code-bias IF-pin pair
 *   ER,<sat|signal>,<last_emit_m>                        # RS-5: eq34 emit re-base frame
 *   RC,<station_idx>,<cb_key>,<value_abs>,<var>,<last_seen_s>  # RS-6: rx-CB carry
 *   SD,<station|sat>,<dev>,<var>,<var_ss>,<t_s>          # RS-6: T5 per-LOS deviation carry
 *   CN,<station_idx>,<cons>,<band>,<C|L>,<signal>        # RS-8: canonical-signal latch
 *   RL,<station_idx>,<cb_key>,<x_lin>                    # RS-9: code-bias linearization point
 *   R,<sat>                                             # pending retro-bump
 *   A,<sat>                                             # retro_active (edge-trigger)
 *   Y,<sat>,<epochs>                                    # URA refloat-inflation horizon
 *   C,<sat>                                             # c1_zoh_active (event edge-trigger)
 *   I,<sat>,<iode>                                      # eph re-level baseline (brdc_rts)
 *   X,<sat>                                             # pending eph-cutover disc bump
 *   SV,<sat>,<vertex_id>                                # invariant carry: satellite vertex id
 *   GG,<const>,<signal>,<group_id>                      # invariant carry: (const, signal) group id
 *   CARRY,...                                           # invariant-carry blob (verbatim;
 *   ...                                                 #  see carry_state)
 *   CEND
 *   EMITTER,v1,...                                      # emitter blob (verbatim,
 *   ...                                                 #  NetworkProductEmitter::serializeState)
 *   EEND
 *   END,<n_lines>                                       # truncation guard (line count)
 * @endcode
 * @c SV, @c GG and the carry blob are written only when the carry blob is present
 * (@c network.state_snapshot_carry).  The @c END line holds the number of lines
 * between the '#' header and itself, blob lines included; a missing or mismatching
 * @c END fails the parse, which the caller treats as the cold-start signal.  Where a
 * record carries @c <station_idx>, station identity is the roster index, valid only
 * under an identical station roster, which the config fingerprint guarantees.
 *
 * The header literal carries the format version: @c v3 when any coupling record
 * (@c QS / @c QL / @c QZ / @c QR, or the @c QK / @c QP side-car) is present, otherwise
 * @c v2 when any §18.10 frame record (@c PA / @c PT / @c CA / @c FA / @c FP / @c FE) is
 * present, otherwise @c v1.  The reader accepts all three, and an absent record group
 * restores empty containers, so an older file is always a legal input and a run that
 * holds neither group writes the v1 bytes unchanged.
 *
 * The RS-3 datum records (@c EQD / @c EQP / @c EQS / @c EQT / @c EQE / @c EQR and
 * @c CBP / @c CBS / @c CBI, docs/SPEC_event_harness.md §21.7) do not advance the header
 * literal.  Like the optional @c var_first field of the @c L record, they are a
 * knob-gated addition whose absence leaves every existing byte unchanged, so an RS-3
 * A/B can diff two snapshots and see only the new rows.  The cost is that a binary
 * predating RS-3 fails an RS-3 file on the unknown record tag instead of on the
 * header.  That is the same documented cold-start signal (and the same throw under
 * @c network.state_snapshot_require), so no safety is lost; only the message differs.
 * Written only under @c network.state_snapshot_datum_capture, so a run that does not
 * arm it writes the v1/v2/v3 bytes unchanged.
 *
 * The RS-5 emit-layer records (@c ER, docs/SPEC_event_harness.md §21.10) follow the
 * RS-3 precedent: rows under the existing header, written only under
 * @c network.state_snapshot_emit_capture.  They hold the state of the output-only
 * engine @c NetworkEmitRebase, its emission frame.  The @c PF / @c PS rows of the removed
 * PB emission filter are no longer parsed: a file that holds them fails to parse and the
 * resume cold-starts.  Nothing in this group enters the
 * warm-resume fingerprint: it changes no estimate, only the level in which the emitted
 * phase biases are expressed.
 *
 * The RS-8 canonical-signal records (@c CN, docs/SPEC_event_harness.md §21.16) follow the
 * same precedent, written only under @c network.state_snapshot_signal_capture.  They
 * hold @c PppModel's per-station, per-(constellation, band, obs-type) canonical signal,
 * which the model decides from one epoch's satellite counts and then locks for the
 * run.  Without them a warm resume re-decides the latch from the resume epoch's
 * counts, which can pick a different signal, change the tracker set, leave an eq34
 * component without its datum receiver, and step the affected phase-bias families at
 * the seam.
 *
 * The RS-6 station-layer records (@c RC / @c SD, §21.13) and the RS-9 code-bias
 * linearization-point record (@c RL, §21.18) follow the same precedent: rows under the
 * existing header, each group behind its own capture knob
 * (@c network.state_snapshot_station_capture, @c network.state_snapshot_rxcb_lin_capture),
 * neither in the fingerprint.  @c RL is the reference that the @c RC entry's inject gate
 * differences against.  A resume that restores @c RC without @c RL judges the restored
 * value against a linearization point re-derived from the resume epoch's code, and the
 * gate can then refuse the carry at a station for a whole leg (§21.18).
 *
 * This module is outside the @c NetworkProductIO replay path: product files hold no
 * snapshot data, and replay reproduces every discontinuity counter without a snapshot.
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
 * Pure data: the driver copies its loop-local state groups in and out, and no field
 * references a driver-internal type, so the module is unit-testable without a
 * running network.
 */
struct NetworkStateSnapshot {
    // --- Metadata --------------------------------------------------------
    int         format_version{1};      ///< Not read or set by serialize() / parse(): the header
                                        ///< literal (v1/v2/v3) follows the record groups present.
    int         snap_week{0};           ///< GPS week of the snapshotted (last processed) epoch.
    double      snap_tow{0.0};          ///< GPS time-of-week of the snapshotted epoch (s).
    std::string wall_utc;               ///< Wall-clock UTC write time (informational, ISO-8601).
    uint32_t    generation_id{0};       ///< Solution-generation id (RTA-3); 0 = never minted.
    uint64_t    config_fingerprint{0};  ///< 64-bit FNV-1a hash of the datum-relevant config fields.

    // --- Run counters (health/reporting continuity + the warmup gates) ----
    uint64_t n_emitted{0};           ///< Emitted-epoch count (drives @c carry_warm and the ramp).
    uint64_t n_skipped{0};           ///< Cumulative skipped epochs.
    uint64_t n_gate_withheld{0};     ///< Cumulative rank/null-space-gate withholds.
    uint64_t n_sigma0_withheld{0};   ///< Cumulative A4 σ̂0-ceiling withholds.
    uint64_t n_runaway_epochs{0};    ///< Cumulative RT0-5 runaway-quarantine epochs.
    uint64_t n_perconst_partial{0};  ///< Cumulative RTA-10 partial-publish epochs.
    uint64_t n_perconst_diffuse{0};  ///< Cumulative RTA-10 diffuse-publish epochs.
    int      iod{0};                 ///< Next IOD-SSR value (monotone across a restart).
    int      prev_week{-1};          ///< Last processed epoch's GPS week (dt spans the outage).
    double   prev_tow{-1.0};         ///< Last processed epoch's TOW (s).
    double   last_emit_content_s{-1.0};   ///< Content time of the last emit (health continuity).
    double   last_health_write_s{-1.0};   ///< Content time of the last health row.

    // --- Quality-ramp and URA EWMA state (RTB-7(b)) -----------------------
    double   ura_sigma0_ewma{1.0};        ///< Network-wide σ̂0 EWMA scaling the emitted URA.
    uint64_t ura_ewma_updates{0};         ///< Number of σ̂0 EWMA updates received (ramp-lift gate).
    bool     ramp_lifted{false};          ///< Warmup URA floor already lifted (product grade reached).
    double   first_emit_content_s{-1.0};  ///< Content time of the first emit (grade-metric anchor).

    // --- Shared carry (D2; driver @c prev_carry) ---------------------------
    /// One carried shared parameter; mirrors the driver's @c CarryPrior, with
    /// @c SharedParamKind stored as its integer value (0 Clock, 1 CodeBias,
    /// 2 PhaseBias, 3 VtecSh).
    struct SharedCarry {
        int         kind{0};        ///< SharedParamKind as int.
        std::string sat;            ///< RINEX satellite id.  VtecSh: "" on layer 0,
                                    ///< "L1" on layer 1 (the Iono-2 layer tag).
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
    std::map<std::string, std::string> last_pivot;  ///< constellation -> last pivot ever held (A1).

    /// Two-state clock-oscillator state; mirrors the driver's @c ClockOsc.
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

    /// Clock-monitor adjacent-epoch baseline: sat -> {Δδtˢ (m), seen_s}.
    std::map<std::string, std::pair<double, double>> prev_clk;

    /// Per-station carried value; mirrors the driver's @c LocalCarry.
    struct Local {
        double value_abs{0.0};   ///< Carried absolute estimate (m).
        double var{0.0};         ///< Posterior conditional variance (m²).
        double last_seen_s{0.0}; ///< Continuous-GPS s when last stashed.
        double var_first{0.0};   ///< First-stash conditional variance (m²); 0 = absent.  An
                                 ///< optional trailing L field, written only when
                                 ///< network.phase_datum_handover_max_var_ratio is armed.  A
                                 ///< file without it restores 0, and the ratio test then
                                 ///< refuses the arc until it re-converges.
    };
    /// (station roster index, sat, signal) -> ambiguity carry.  Index validity
    /// is guaranteed by the roster fields inside the config fingerprint.
    std::map<std::tuple<int, std::string, std::string>, Local> local_carry;
    std::map<int, Local> zwd_carry;  ///< station roster index -> ZWD carry (B9).

    std::map<std::string, int> ura_refloat_horizon; ///< sat -> post-refloat URA-inflation epochs left.
    std::set<std::string> ura_retro_sats;  ///< Sats with a B1 retro-bump pending for the next emit.
    std::set<std::string> retro_active;    ///< Currently-flagged sats (retro edge-trigger).
    std::set<std::string> c1_zoh_active;   ///< T5/R6 c1-ZOH event edge-trigger (log-only dedup).

    // --- Ephemeris-cutover re-level state, brdc_rts only (RTF-2) ----
    /// sat -> broadcast IODE the carried clock state is currently referenced to
    /// (the eph-cutover re-level baseline).  Empty in sp3 mode or in an older snapshot
    /// without @c I rows; the first brdc_rts epoch then re-seeds it, so absence is safe.
    std::map<std::string, int> relevel_iode;
    /// sats with a pending forced eph-cutover discontinuity (prior eph version
    /// evicted before Δ_eph could be computed), awaiting their next emit.
    std::set<std::string> eph_cutover_disc_pending;

    // --- Invariant DD carry, branch-faithful resume (M1, RTB-7) ------
    /// ArcKey vertex numbering that the carry blob references (network.state_snapshot_carry).
    /// @c carry_sat_vertex maps a satellite id to its stable (first-seen-order) vertex id,
    /// and @c carry_group_id maps a (constellation, signal) to its stable group id.  Both
    /// are restored so that the reconstructed chords reference the same satellites and
    /// groups at the next epoch.  Station vertices are the roster index, which the
    /// fingerprint keeps stable, so they need no map.  Empty unless the knob is armed (it
    /// requires carry_invariant); when absent, the invariant carry cold-starts.
    std::map<std::string, int> carry_sat_vertex;
    std::vector<std::tuple<char, std::string, int>> carry_group_id;  ///< (const, signal, group id).
    /// Verbatim @c NetworkInvariantCarryDriver::serializeCarry blob (its @c CARRY header
    /// through its @c CEND terminator, newline-separated): the carried z-store
    /// (per-component DD means, dense @f$P^z@f$, tree/chord/vertex structure), which is
    /// the branch that a branch-faithful resume continues.
    std::string carry_state;

    // --- Resume frame state (format v2; SPEC_synthetic_minimum_network §18.10) ----
    //
    // Realization-frame bookkeeping.  Without it the first warm epoch's phase-anchor
    // election has no incumbent for any (sat, signal) family, so it marks every family
    // as re-referenced, erases every restored leveling carry, marginalizes their D2
    // phase-bias prior and re-pins the frame at fresh code seeds; the Q3 clock anchor
    // folds loose for its re-couple horizon; and the CLK-R held frame restarts at zero.
    // The §18.9 seam census attributes the whole post-resume transient to this missing
    // state.  Captured whenever a snapshot is written, with no knob (as the Z rows are);
    // a run that holds none of this state writes the v1 format byte for byte.  A reader
    // restores the records into maps that nothing reads unless
    // `network.resume_frame_restore` is armed (a forward-only resume response policy,
    // not fingerprinted).
    /// "sat_signal" ambiguity key -> station name of the sticky phase anchor: the
    /// incumbent that the next election must find to keep the family's frame.
    std::map<std::string, std::string> anchor_incumbent;
    /// "sat_signal" -> R2 sticky continuous pin target (m): the absolute value the
    /// incumbent anchor is realized at.  Absent ⇒ the 0-frame (a cold pin realizes
    /// the same frame as long as the incumbent is kept).
    std::map<std::string, double> anchor_pin_target;
    /// Per-satellite state of the guarded temporal clock anchor; mirrors the
    /// driver's @c ClockAnchorSat.  With it restored, the first post-resume fold runs
    /// anchored over the true elapsed time instead of loose from a first sighting.  The
    /// stale test still runs on @c t_last, so a resume older than the anchor's own max
    /// gap still decouples the anchor.
    struct ClockAnchorSat {
        double t_last{0.0};        ///< continuous-GPS s of the last fold.
        double last_value{0.0};    ///< prior mean folded at that epoch (m).
        double last_var_eff{0.0};  ///< prior variance folded at that epoch (m²).
        int    in_band{0};         ///< consecutive in-band epochs while loose.
        bool   anchored{false};    ///< true when anchored, false when loose.
    };
    std::map<std::string, ClockAnchorSat> clock_anchor;  ///< sat -> anchor state.
    /// CLK-R stage-1 held-frame engine state (@c NetworkClockEmitRebase): the
    /// per-constellation frame accumulator A_c, the per-sat previous held value and
    /// last-seen stamp, and the epoch count, so that the F1 `resume_seam` datum mark
    /// (keyed on a fresh engine) does not fire when the frame continues.
    std::map<char, double> clk_frame_acc;                        ///< constellation -> A_c (m).
    std::map<std::string, std::pair<double, double>> clk_frame_prev; ///< sat -> {last held (m), last seen (s)}.
    long long clk_frame_epochs{0};                               ///< rebaseEpoch count.
    // --- Coupling regressors (format v3; SPEC_synthetic_minimum_network §18.12) ---
    //
    // Second half of the resume frame restore.  A leveling carry restored from
    // (value, var) alone (v1/v2) is injected as a plain diagonal row at the pure
    // conditional variance, a combination that no continuous epoch produces: the legacy
    // diagonal carry is stiff at the marginal variance, and the coupling row is stiff at
    // the conditional variance but shared-neutral (its @f$A_s = M_r@f$ cancels the
    // reduced-normals contribution).  Such rows out-vote the Q3 anchored clock priors,
    // and anchors trip at the first warm fold (§18.11).
    /// Identity of one shared-border column, plus the two quantities that depend on the
    /// column and not on the carried row.  A POD mirror of the driver's @c CoupledShared
    /// without the coefficient, with @c SharedParamKind and the SH trig char stored as
    /// their integer values, as in @ref SharedCarry.
    ///
    /// @note @c p_prev == 0 carries meaning: a datum-pinned or ensemble-gauge column is
    ///       "exactly known" (networkHarvestColumn, the former harvest lambda,
    ///       src/core/network/carry/LocalCarryHarvest.cpp).  A reader must not treat it as
    ///       absent or re-derive it: the κ shrink is guarded on @c p_prev > 0 (the
    ///       CouplingRowBuilder row builders, Coupling.cpp), so a re-derived value
    ///       would shrink a column that the datum re-applies exactly and change the row's
    ///       gauge sum.
    /// @note @c sh_layer is a field of its own, not the "L1" layer tag that the @c D record
    ///       stores in its sat field (networkWriteStateSnapshot in
    ///       StateSnapshotWrite.cpp and networkTryWarmResume in
    ///       WarmResume.cpp): @c resolveSharedCol resolves through
    ///       @c vtecCoeffColumn(n,m,trig,sh_layer), so a lost layer would bind a layer-1
    ///       coefficient to the live layer-0 column.
    struct CouplingCol {
        int         kind{0};      ///< SharedParamKind as int: 0 Clock, 1 CodeBias, 2 PhaseBias, 3 VtecSh.
        std::string sat;          ///< satellite id (empty for VtecSh).
        std::string signal;       ///< signal id (empty for Clock and VtecSh).
        int         sh_n{0};      ///< SH degree (VtecSh).
        int         sh_m{0};      ///< SH order (VtecSh).
        int         sh_trig{'C'}; ///< SH cos/sin as the char's int value (67/83).
        int         sh_layer{0};  ///< SH shell index (VtecSh; 0 = legacy layer).
        double      s_prev{0.0};  ///< @f$\hat s_{\text{prev},j}=x_s(j)@f$ at harvest (m, datum-realized).
        double      p_prev{0.0};  ///< @f$P_{\text{prev},jj}@f$ at harvest (m², datum frame).  0 = pinned/exact.
    };
    /// Dense, first-encounter-ordered dictionary of every shared column that any carried
    /// row's regression references.  Its index is the @c QL / @c QZ entry id.
    ///
    /// @note The dictionary key is (identity, @c s_prev, @c p_prev), not the identity
    ///       alone.  @c networkHarvestColumn (the former harvest lambda harvestCoupling,
    ///       LocalCarryHarvest.cpp) assigns both values from the shared column @c j,
    ///       so the rows of one harvest epoch agree.  A station whose harvest fails skips the
    ///       stash and keeps an older coupling, so two rows can hold different vintages of
    ///       one identity, and a dictionary keyed on identity alone would restore the
    ///       older rows against the wrong frame.
    std::vector<CouplingCol> coupling_dict;
    /// (station roster index, sat, signal), the key of @ref local_carry, mapped to the
    /// row's regression as (dictionary id, @f$M_{rj}@f$) pairs in harvest order.  The
    /// order is kept because it affects the result: @c CouplingRowBuilder
    /// (Coupling.cpp) accumulates the row RHS and the family deficit in it.
    std::map<std::tuple<int, std::string, std::string>,
             std::vector<std::pair<int, double>>> coupling_rows;
    /// station roster index -> the ZWD carry's regression (same shape).  ZWD coupling is
    /// harvested (networkHarvestZwd, network/carry/ZwdCarry.cpp) and injected
    /// (networkPassB_, ApplicationNetworkAssembly.cpp), so a Z row restored without its
    /// regression has the same defect as an L row.
    std::map<int, std::vector<std::pair<int, double>>> coupling_zrows;
    /// The write epoch's clock-monitor refloat set
    /// (@c NetworkCarryState::prev_clock_refloat_sats).  A hard-refloated
    /// satellite is not stashed, so it has no @c D row, and no other record in this file
    /// carries the flag.  Without this set the first warm epoch's serial pre-pass
    /// (networkPassB_, ApplicationNetworkAssembly.cpp) writes no @c tu_reinit_hist mark for
    /// the satellite, and every restored term on it folds at full weight onto a re-drawn
    /// column.
    std::set<std::string> clock_refloat_sats;
    /// The C1 dense D2 covariance side-car (@c SharedCarryCov; §18.12 (2a)): identity keys
    /// in row order (@c QK records) and the lower triangle of P per row (@c QP records:
    /// row i holds P(i,0..i)).  It is the other half of the coupling restore: with the
    /// coupling rows conditional from the first warm fold, the border they condition on
    /// must also fold dense from the first fold, or the diagonal asymmetry measured in
    /// §18.12 remains.  Written only under network.state_snapshot_coupling_capture on a
    /// network.shared_carry_full_cov posture, and restored behind
    /// network.resume_coupling_restore.  Empty on every other file.
    std::vector<std::string> carry_cov_keys;
    std::vector<std::vector<double>> carry_cov_rows;
    // --- Datum memories (RS-3; SPEC_event_harness §21.7) ---------------------
    /// The eq34 chain datum's spanning tree at the last epoch, written under
    /// @c network.state_snapshot_datum_capture and restored behind
    /// @c network.resume_datum_restore.  @ref anchor_incumbent (a frame record) holds the
    /// satellite-side parent edges of the same tree; these maps hold the receiver side
    /// and the per-component root vote, and together they are what the election's
    /// sticky-tree seed continues.  Empty on every other file, and a resume then
    /// re-elects from the cold arc registry.
    ///
    /// @c eq34_datum_rx: "<cons>|<signal>" -> sticky datum receiver r1 (station name).
    std::map<std::string, std::string> eq34_datum_rx;
    /// "<station>|<cons>|<signal>" -> that receiver's parent-edge satellite.
    std::map<std::string, std::string> eq34_rx_parent;
    /// "<sat>_<signal>" -> that satellite's component datum receiver (station name).
    std::map<std::string, std::string> eq34_sat_root;
    /// (station roster index, "<sat>_<signal>") -> last epoch's pin target (cycles).
    /// Keyed per station on purpose: an interior satellite carries several pins, each at
    /// a different station with its own target.  An ambiguity-only key collides, and the
    /// resulting pin moves feed back into the rounded targets, which then diverge, so the
    /// key must survive serialization intact.
    std::map<std::pair<int, std::string>, double> eq34_prev_target;
    /// (station roster index, sat) -> last epoch's arc elevation (deg), the
    /// rising-vs-setting input of the longevity election.  Empty unless
    /// network.phase_datum_eq34_longevity_election was armed on the writer.
    std::map<std::pair<int, std::string>, double> eq34_prev_el;
    /// {station roster index, "<sat>_<signal>"}: tree edges retained across a gap
    /// because their local carry was still alive.  Empty unless
    /// network.phase_datum_eq34_extend_only was armed on the writer.
    std::set<std::pair<int, std::string>> eq34_retained_edges;
    /// The time-linked code-bias datum's previous-epoch solved split,
    /// "<cons>|<signal>" -> satellite -> value (m), and the set of satellites whose split
    /// has ever been harvested.  Restored behind the same read knob.  With them present,
    /// the resume leaves @c code_bias_datum_resumed_ false, so a re-seed disc-bumps only
    /// the satellites that actually re-seed instead of every satellite.  Empty unless
    /// network.code_datum_time_linked was armed on the writer.
    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::set<std::string> code_bias_sat_seen;
    /// The IF-pin reference pair (satellite -> {sig1, sig2}) that produced the values in
    /// @ref code_bias_prev: the third container of the same kept-epoch lifecycle, and the
    /// one that gates the IF-pin half of the datum.  The datum's RHS is a continuity term
    /// only when the pair is unchanged from the last kept epoch and both signals had a
    /// previous value (`pair_same` in Sbasis.cpp), so an empty map reads as "the
    /// pair changed" for every satellite: the split re-seeds to 0 and, with
    /// @ref code_bias_sat_seen restored, every satellite disc-bumps.  Without this map the
    /// IF-pin half of the restore is inert (standing invariant #4).  Restoring it makes
    /// the same continuity claim as restoring the values (a resume's previous kept epoch
    /// is the writer's last kept epoch), and a pair that did change still trips the test
    /// and still bumps.
    std::map<std::string, std::pair<std::string, std::string>> code_bias_ifpin_pair;
    /// True when any RS-3 datum record is present.  A predicate of its own, like
    /// @ref hasCouplingState: the restore site reports and warns about the two halves
    /// (eq34 tree, code-bias memory) separately, and each half is judged inert from its
    /// own records only.
    bool hasDatumState() const {
        return !eq34_datum_rx.empty() || !eq34_rx_parent.empty()
            || !eq34_sat_root.empty() || !eq34_prev_target.empty()
            || !eq34_prev_el.empty() || !eq34_retained_edges.empty()
            || !code_bias_prev.empty() || !code_bias_sat_seen.empty()
            || !code_bias_ifpin_pair.empty();
    }
    /// True when any coupling record is present (the writer then emits the v3 header).
    ///
    /// @warning Kept separate from @ref hasFrameState, which also gates the resume-side
    ///          frame restore (networkTryWarmResume, WarmResume.cpp).  Widening
    ///          hasFrameState to the coupling records would let a coupling-bearing snapshot
    ///          with no PA/PT/CA rows claim frame state it does not have: the restore would
    ///          proceed with an empty @c anchor_incumbent, the QUEUE-1(d) erase would still
    ///          fire and remove the restored carries, and the coupling restore would have
    ///          no effect.
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
    // --- Emit-layer state (RS-5; SPEC_event_harness §21.10) -----------------
    //
    // The output-only engine between the estimator's raw phase-bias state and the wire
    // (rebaseEpoch, called from EmitStore.cpp) is a local of runNetwork
    // (ApplicationNetwork.cpp), so without these records a warm resume cold-starts it:
    // the leg's first emitted epoch passes the raw solve-frame value
    // through, while the continuous run's wire at the same epoch carries the accumulated
    // re-base.  At the field seam the emitted leg-minus-seed difference equals the raw
    // one plus the difference of the two emit layers, `emitted = raw + (leg layer − seed
    // layer)` (§21.10).  Written under network.state_snapshot_emit_capture, restored
    // behind network.resume_emit_restore; empty on every other file.
    /// Mirror of @c NetworkEmitRebase::last_emit_: "sat|signal" -> previous re-based
    /// emitted bias (m), the emission frame the next epoch re-bases onto (@c ER rows).
    std::map<std::string, double> emit_rebase_last;
    /// True when any RS-5 emit-layer record is present.  A predicate of its own, like
    /// @ref hasDatumState and @ref hasCouplingState: the restore site judges the re-base
    /// frame inert from its own records only.  Not folded into @ref hasFrameState: that
    /// predicate gates the resume-side frame restore, and widening it would let an
    /// emit-only snapshot claim frame state it does not have.
    bool hasEmitState() const {
        return !emit_rebase_last.empty();
    }
    // --- Station-layer memories (RS-6; SPEC_event_harness §21.13) -----------
    //
    // Two per-station cross-epoch memories.  Without these records a warm resume
    // restarts both cold, while the estimator's own level still moves at the first
    // post-resume epoch (§21.13):
    //   * the rx-CB carry (@c NetworkCarryState::rx_cb_carry): the receiver code-bias
    //     posterior per (station, CB state key), with RW q 1e-9 m²/s.  A cold restart
    //     re-splits code bias, iono and clock per station and signal, and its 10σ
    //     inject gate then withholds more priors on the leg than on the seed; and
    //   * the T5 / Iono-2 per-LOS SH representation-error deviation carry
    //     (@c NetworkCarryState::sh_dev_carry), whose cold seeds at the first leg epoch
    //     cause the a₀₀ transient.
    // Written under network.state_snapshot_station_capture, restored behind
    // network.resume_station_restore; empty on every other file.
    /// (station roster index, CB state key) -> receiver code-bias carry (@c RC rows).
    /// Serialized with the same @ref Local fields as the @c Z rows (value_abs, var,
    /// last_seen_s): the rx-CB harvest stashes a bare
    /// @c LocalCarry{value_abs, var, pl_now_s} and its inject reads only those three,
    /// so there is no fourth field to carry.  Index validity is guaranteed by the
    /// roster fields inside the config fingerprint, as for @ref local_carry /
    /// @ref zwd_carry.
    std::map<std::pair<int, std::string>, Local> rx_cb_carry;
    /// (station roster index, CB state key) -> the station @c StateManager's code-bias
    /// linearization point @c x_lin(cb), in metres (@c RL rows, written alongside the
    /// @c RC rows under @c network.state_snapshot_rxcb_lin_capture; RS-9,
    /// SPEC_event_harness §21.18).
    ///
    /// The RC row carries the rx-CB carry's numerator; this is the reference that its
    /// inject gate differences against (@c cb_innov = @c value_abs − @c x_lin(cb),
    /// in @c networkPassB_, ApplicationNetworkAssembly.cpp).  It is seeded once per
    /// station by @c MeasurementModel::initializeCodeBiases behind a one-shot latch and
    /// never written back.  A resume that restores the carry without it judges the
    /// restored value against a linearization point re-derived from the resume epoch's
    /// code, and the gate then refuses carries that a continuous run accepts (§21.18).
    /// It has its own map and its own knob, not a fourth field of @ref Local, because it
    /// is a property of the station's state vector, not of the carry entry: a station
    /// can have a linearization point with no carry (the carry is cold), and the row
    /// must still travel.  Not folded into @ref hasStationState, for the same reason the
    /// emit and datum halves keep their own predicates: an absent RL block must read as
    /// inert for the RL restore alone.
    std::map<std::pair<int, std::string>, double> rx_cb_lin;
    /// One T5 deviation carry; a POD mirror of the driver's @c ShDevCarry, as @ref Osc
    /// mirrors @c ClockOsc (the snapshot module names no engine type).  All four
    /// members are scalars, so the @c SD row is the whole struct in declaration order
    /// and the restore needs no re-derivation.
    struct ShDev {
        double dev{0.0};     ///< Carried GM deviation (vertical TECU, v2 units).
        double var{0.0};     ///< Its posterior variance (vertical TECU²).
        double var_ss{0.0};  ///< The GM steady-state variance = representation_sigma_tecu².
        double t_s{0.0};     ///< Continuous-GPS s of the last harvest; ≤ 0 marks a never-harvested (cold) entry.
    };
    std::map<std::string, ShDev> sh_dev_carry;  ///< "<station>|<sat>" -> deviation carry.
    /// Clock level monitor state (G5): satellite -> (the code-only information of its level in
    /// the constellation contrast, 1/m^2; the epochs it accumulated), @c LB rows.  Written
    /// whenever the level monitor (network.clock_level_monitor) ran; read back by a leg's
    /// raise on restore (network.clock_level_restore_raise).  Empty otherwise.
    std::map<std::string, std::pair<double, int>> level_info;
    /// True when any RS-6 station-layer record is present.  A predicate of its own, like
    /// @ref hasEmitState / @ref hasDatumState / @ref hasCouplingState: the restore site
    /// reports and warns about the two halves (rx-CB carry, deviation carry) separately,
    /// and each half is judged inert from its own records only.
    bool hasStationState() const {
        return !rx_cb_carry.empty() || !sh_dev_carry.empty();
    }
    // --- Canonical-signal latch (RS-8; SPEC_event_harness §21.16) -----------
    //
    // The per-station, per-(constellation, band, obs-type) canonical signal that
    // PppModel::decideConstellationCanonicals picks from the satellite counts of the
    // station's first processed epoch and then locks for the run.  Without these
    // records a warm resume re-decides it from the resume epoch's counts, which can
    // flip a station's pick for a band (for example L2X to L2W).  That changes the
    // tracker set, can leave an eq34 component without its datum receiver, and steps
    // the affected phase-bias families at the seam.  Written under
    // network.state_snapshot_signal_capture, restored behind
    // network.resume_signal_canon_restore; empty on every other file.
    /// One locked canonical pick; a POD mirror of @c PppModel::CanonicalSignal, as
    /// @ref ShDev mirrors @c ShDevCarry (the snapshot module names no engine type).
    struct CanonSignal {
        std::string constellation;   ///< System display name, e.g. "GPS".
        std::string band;            ///< Band name, e.g. "L2".
        char        obs_type{ 'C' }; ///< 'C' (code) or 'L' (phase).
        std::string signal;          ///< Locked RINEX code, e.g. "L2X".
    };
    /// Station roster index -> its locked canonicals, in the writer's export order
    /// (deterministic; the model iterates nested @c std::map).  Index validity is
    /// guaranteed by the roster fields inside the config fingerprint, as for
    /// @ref local_carry / @ref zwd_carry / @ref rx_cb_carry.
    std::map<int, std::vector<CanonSignal>> canonical_signals;
    /// True when any RS-8 canonical-signal record is present.  A predicate of its own,
    /// like @ref hasStationState / @ref hasEmitState: an absent group reads as inert for
    /// itself alone and never takes another group's verdict.
    bool hasCanonicalSignalState() const { return !canonical_signals.empty(); }
    // --- Emitter layer -----------------------------------------------------
    /// Verbatim @c NetworkProductEmitter::serializeState blob (its own
    /// @c EMITTER header through its @c EEND terminator, newline-separated).
    /// Restored via @c NetworkProductEmitter::restoreState; counters, bias
    /// baselines, first-sighting sets, RT0-5 pending forced-bump flags and
    /// RTA-11 last-seen stamps all round-trip exactly.
    std::string emitter_state;
};

/**
 * @brief Reason a warm resume was refused (the HB-1 classification).
 *
 * A machine-readable companion to the gate's prose, so that the
 * `network.state_snapshot_require` policy is decided on the class rather than on
 * a substring of an error message, and so that the one refusal with a
 * harness-specific remedy (@ref WarmResumeRefusal::NonPositiveAge: a leg
 * relaunched in place after it overwrote its own seed) can name that remedy.
 */
enum class WarmResumeRefusal {
    None = 0,        ///< Not a refusal (the gate passed).
    MissingFile,     ///< @c network.state_snapshot_path does not exist.
    ParseFailure,    ///< The file exists but did not parse.
    NoLiveEpochs,    ///< No epoch to resume against (start_offset past the end).
    Fingerprint,     ///< Datum-relevant config changed since the snapshot was written.
    NonPositiveAge,  ///< First live epoch is at or before the snapshot epoch.
    Stale,           ///< Age beyond the carry-validity horizon.
    EmitterRestore,  ///< The emitter section failed to restore (all-or-nothing).
    Other            ///< Any other refusal (one without a class of its own).
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
     *        file banner).  Deterministic: maps and sets are written in key order,
     *        vectors (the shared carry among them) in element order.
     * @param os   Destination stream (caller owns; no flush performed).
     * @param snap Snapshot to serialize.
     */
    static void serialize(std::ostream& os, const NetworkStateSnapshot& snap);

    /**
     * @brief Parse a snapshot from @p is.  Strict: an unrecognized header, an unknown
     *        record tag, a malformed or out-of-range field, a missing @c M / @c N /
     *        @c U line, a truncated or duplicated emitter or carry section, a coupling
     *        row (@c QL / @c QZ) that does not bind to its @c L / @c Z row and to the
     *        dictionary, a @c QK / @c QP count mismatch, or a missing or mismatching
     *        @c END line count fails the parse (the caller's cold-start signal).
     *        @p out is written only on success.
     * @param is  Source stream.
     * @param out Parsed snapshot (untouched on failure).
     * @param err Optional human-readable failure reason.
     * @return True on a complete, well-formed parse.
     */
    static bool parse(std::istream& is, NetworkStateSnapshot& out, std::string* err = nullptr);

    /**
     * @brief Write @p snap to @p path, replacing an existing file atomically.
     *
     * Serializes to <tt>path + ".tmp"</tt>, closes it, then replaces @p path
     * via @c std::filesystem::rename.  On MSVC, @c rename maps to
     * @c MoveFileExW with @c MOVEFILE_REPLACE_EXISTING, and the temp file sits
     * beside the target on the same volume, so an existing target is replaced
     * atomically: a crash before the rename leaves the previous good snapshot
     * untouched (only the @c .tmp is partial), and a crash after it leaves the
     * new complete snapshot.  The previous snapshot is never opened or modified
     * in place.  If an implementation refuses to rename onto an existing target
     * (POSIX semantics differ from some Windows layers), a fallback removes the
     * target and renames again.  The fallback is not atomic: a crash between the
     * remove and the rename leaves no file at @p path, with the complete
     * <tt>path + ".tmp"</tt> still on disk.  MSVC does not reach the fallback.
     * A failed write or rename removes the @c .tmp.
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
     * A warm resume requires both:
     *  1. @c snap.config_fingerprint == @p live_config_fingerprint: the
     *     datum-relevant config (station roster order, pivot pins, carry q's,
     *     clock-model parameters, emitter channel toggles) is unchanged; and
     *  2. the snapshot's product-time age at the first live epoch,
     *     <tt>first_live_epoch_s − (snap_week·604800 + snap_tow)</tt>, is
     *     strictly positive (resuming at or before the snapshotted epoch would
     *     fold that epoch's carry twice) and at most @p carry_horizon_s (the max
     *     of the per-key gap bounds; past it every carried key would be dropped
     *     or reseeded downstream anyway, so a warm restore gains nothing and only
     *     risks a stale datum).
     *
     * The tests run in that order, and the first failure sets @p why.  Per-key
     * staleness inside the horizon is not judged here, on purpose: the existing
     * downstream gap-inflation, max-gap-drop and t_last-reseed logic evaluates
     * each restored key at the first resumed epoch.
     *
     * @param snap                    Parsed snapshot.
     * @param live_config_fingerprint The resuming run's fingerprint.
     * @param first_live_epoch_s      Continuous-GPS seconds of the first epoch
     *                                the resuming run will process.
     * @param carry_horizon_s         Carry-validity horizon (s of product time).
     * @param reason                  Optional explanation (both outcomes).
     * @param why                     Optional machine-readable refusal class, so that
     *                                a caller can act on the reason instead of
     *                                matching the prose (set to
     *                                @c WarmResumeRefusal::None when the gate passes).
     * @return True when a warm resume is allowed.
     */
    static bool warmResumeAllowed(const NetworkStateSnapshot& snap,
                                  uint64_t live_config_fingerprint,
                                  double first_live_epoch_s,
                                  double carry_horizon_s,
                                  std::string* reason = nullptr,
                                  WarmResumeRefusal* why = nullptr);

    /**
     * @brief Decide what a refused warm resume means for the run (HB-1).
     *
     * `network.state_snapshot_require` exists so that a leg that cannot complete its
     * warm resume stops, instead of silently becoming a cold run that is then graded
     * as warm.  Every refusal path (a missing file, a parse failure, a fingerprint
     * mismatch, a non-positive age, a stale age, a failed emitter restore) goes
     * through this function, so "require" means the same thing on every branch
     * (docs/SPEC_event_harness.md §6b).
     *
     * @param require  @c network.state_snapshot_require.
     * @param why      The refusal class.
     * @param detail   The gate's own prose (appended verbatim).
     * @return The fatal message, which the caller must log and throw, when
     *         @p require is set and @p why is not @c WarmResumeRefusal::None;
     *         otherwise an empty string, and the run may fall through to a cold
     *         start.
     */
    static std::string warmResumeRefusalMessage(bool require, WarmResumeRefusal why,
                                                const std::string& detail);

    /// 64-bit FNV-1a hash of @p s: the config-fingerprint hash primitive, with the
    /// same constants as the L15 manifest's config-bytes hash.
    static uint64_t fnv1a64(const std::string& s);

    /// Current wall-clock UTC as "YYYY-MM-DDTHH:MM:SSZ" (snapshot metadata).
    static std::string utcNowIso();
};

/**
 * @brief Startup quality-ramp lift rule (RTB-7(b)); a pure function.
 *
 * The warmup URA floor (@c network.warmup_ura_floor_m) stays applied until the
 * producer is at product grade, which requires both:
 *  - @p n_emitted ≥ max(@p warmup_epochs, @p phase_leveling_warmup_epochs), so
 *    that the carry population is warm (the phase-leveling warmup gates when
 *    carries start at all, so counting emitted epochs alone below it would lift
 *    the floor while the solution is still carry-cold); and
 *  - the σ̂0 EWMA has received at least @p warmup_epochs updates
 *    (@p ewma_updates), or the EWMA is structurally unavailable
 *    (@p ewma_structurally_available false: the RTB-6 sentinel condition,
 *    @c emit_ura && @c raim && @c phase_leveling not all armed).  In that case
 *    there is no stabilization statistic to wait for, and the emitted-epoch count
 *    alone decides.
 *
 * Counts run through the previous epoch: the producer evaluates the rule before
 * emitting the current epoch, with the same indexing as the earlier floor.
 *
 * @param n_emitted                    Epochs emitted before the current one.
 * @param warmup_epochs                @c network.warmup_epochs.
 * @param phase_leveling_warmup_epochs @c network.phase_leveling_warmup_epochs.
 * @param ewma_updates                 σ̂0 EWMA updates received so far.
 * @param ewma_structurally_available  Whether the EWMA can update at all this
 *                                     run (emit_ura && raim && phase_leveling).
 * @return True when the degraded warmup floor lifts (product grade reached).
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
