#pragma once
/**
 * @file NetworkGfArcJump.hpp
 * @brief The arc-scoped geometry-free jump trigger (card ARL, slip-ladder defect 1): a
 *        pre-solve slip test per (station, satellite) dual-frequency arc, drained in
 *        PASS A beside the IF-phase triple-difference driver.
 *
 * ## The blind spot it closes
 *
 * The network's pre-solve slip layer has two time-difference detectors, MW and the
 * pooled IF-phase triple difference (IF-TD), plus the receiver's LLI.  Three slip
 * classes pass all of them (docs/refound/ARL_integer_ambiguity_ladder_2026-10-09.md,
 * section 12, the slip ladder):
 *   - an equal count on both bands, (1,1): MW does not move, and the IF step,
 *     0.1070 m, sits exactly on the IF-TD floor (rung S4b, detected about half the
 *     time);
 *   - the 60/77 family, (4,5), (3,4), (7,9), (8,10): one wide-lane cycle or two, an IF
 *     step of a few centimetres, and a geometry-free step of 0.41 to 0.92 m that the
 *     free slant ionosphere absorbs (rung S8, the only rung that produced wrong
 *     integers);
 *   - the epoch after an IF-TD flag: the detector restarts the satellite's chain, so a
 *     slip repeated one epoch later has no sample (rung S7, repetition 2).
 * The one pre-fit witness of all three is the raw geometry-free phase step.
 *
 * ## The test
 *
 * Per arc, with GF = lambda1 L1 - lambda2 L2 in metres (@c RawSlipObs::gf_m) and the
 * one-epoch step d(t) = GF(t) - GF(t-1):
 *
 *     r(t) = d(t) - d(t_c),   t_c the arc's last CLEAN step,
 *     flag when |r(t)| > max(floor_m, k * 1.4826 * MAD(the arc's recent clean r)).
 *
 * The reference is the last clean step, not the previous one.  A flagged step never
 * becomes the reference, so a slip repeated at the next epoch still differs from it
 * (rung S7), while an isolated slip flags once: the step after it is ordinary and
 * matches the reference again.  The scale term is off for k = 0, and while the arc
 * holds fewer than @ref GfArcJumpParams::min_count clean values.
 *
 * An arc is consecutive processed epochs on the same primary pair with the same step
 * span.  A gap, a pair change or a span change restarts it: the restart sample opens
 * the arc, the next step seeds the reference untested, and testing begins at the third
 * sample.  A slip inside the seed step becomes part of the reference, so the ordinary
 * step after it departs from the reference by the slip (opposite sign) and flags one
 * epoch late.
 *
 * A persistent change of the GF rate (an ionospheric front) is indistinguishable from
 * a repeated slip for one epoch, and against a locked reference it would flag every
 * epoch.  After @ref kGfArcJumpMaxFlagRun consecutive flags the reference is therefore
 * re-seeded from the next step, which bounds a rate change at that many flags.
 *
 * ## The response
 *
 * A flag with a warm carry frees the pair's two bands as the IF-TD pair refloat does:
 * the pending-reseed mark, the free-epoch latch (@c RelevelEpochState::free_ambs with
 * the cause bit @c kCauseGfJump), the Card-4 funnel insert under carry_invariant, and
 * the void of any outstanding bridge pend of that (station, satellite).  A band the
 * common-band verdict refuses stays pinned (the same predicate every drain consults).
 * A band an earlier drain already freed this epoch takes only the cause bit, so the
 * record names every tier that saw the slip while the response, and any pend that
 * drain created, is left as it was.  A cold carry makes the flag diagnostic.
 *
 * ## State and threading
 *
 * The knob snapshot is @c SlipQcParams::gf_jump, the per-arc state
 * @c RawSlipHistory::gf_arcs, the run counters @c SlipQcCounters::gf_jump.  The drain
 * runs serially in station order (deterministic).  The state is not snapshotted, like
 * the rest of @c RawSlipHistory: a resumed run starts every arc cold.
 */

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct NetStation;
struct SyncedEpoch;
struct NetworkRunTraces;
struct RelevelEpochState;
struct RelevelPend;
struct CommonBandEpoch;
struct SlipQcChannel;
struct NetworkRunEndView;

/// Consecutive flags on one arc after which its reference is re-seeded from the next
/// step.  Two is the smallest cap that still catches a slip repeated at the next epoch
/// (ladder rung S7), and it bounds a persistent GF-rate change at two flags.
inline constexpr int kGfArcJumpMaxFlagRun = 2;

/// Tolerance (s) on the step span: a step whose span differs from the reference step's
/// by more than this (a skipped processed epoch, a decimation change) is not
/// comparable, and the arc restarts.
inline constexpr double kGfArcJumpSpanTolS = 1.0e-3;

/// Knob snapshot of the trigger (network.cycle_slip.gf_arc_jump*), taken once per run.
struct GfArcJumpParams {
    bool   armed{false};    ///< Master arm (network.cycle_slip.gf_arc_jump); default off, byte-identical.
    double floor_m{0.04};   ///< Physical floor on |r| (m) (gf_arc_jump_m).  0.04 m sits below the (1,1) GF step of 0.0539 m and above the sim control's largest clean |r|, 0.0343 m.
    double k{0.0};          ///< Multiplier on the arc's robust scale (gf_arc_jump_k); 0 turns the scale term off.
    int    window{20};      ///< Clean r values kept per arc for the scale (gf_arc_jump_window).
    int    min_count{10};   ///< Clean values an arc must hold before the scale term applies (gf_arc_jump_min_count).
};

/// Cross-epoch state of one (station, satellite) arc.
struct GfArcJumpArc {
    long long   last_epoch{-1};  ///< Processed-epoch index of the last GF sample; -1 before the first.
    double      last_t_s{0.0};   ///< Its time (continuous GPS seconds).
    std::string sig1, sig2;      ///< The primary pair the GF was formed on.
    double      gf_m{0.0};       ///< The last GF level (m).
    bool        has_ref{false};  ///< A reference step exists (the arc's last clean step).
    double      ref_step_m{0.0}; ///< The reference step d(t_c) (m).
    double      ref_span_s{0.0}; ///< The reference step's span (s).
    int         flag_run{0};     ///< Consecutive flagged steps.
    std::deque<double> clean_r;  ///< The last @ref GfArcJumpParams::window clean r values (m), oldest first.
};

/// What one call of @ref gfArcJumpStep decided.
struct GfArcJumpVerdict {
    bool   tested{false};     ///< r was formed and thresholded (an opportunity).
    bool   flagged{false};    ///< |r| exceeded the threshold.
    bool   restarted{false};  ///< Continuity broke (gap, pair change, span change) on an arc that had a sample; the arc restarted.
    bool   reseeded{false};   ///< This flag completed a run of @ref kGfArcJumpMaxFlagRun; the next step seeds a fresh reference.
    double step_m{0.0};       ///< d(t) (m); 0 when no step was formed.
    double ref_step_m{0.0};   ///< d(t_c) the step was compared with (m); valid when @ref tested.
    double r_m{0.0};          ///< r(t) (m); valid when @ref tested.
    double thr_m{0.0};        ///< The threshold applied (m); valid when @ref tested.
    double scale_m{0.0};      ///< The robust scale used (m); 0 when the scale term was off.
    int    n_clean{0};        ///< Clean values the scale was formed from; 0 when the scale term was off.
};

/**
 * @brief Robust scale of an arc's clean r values: 1.4826 times the median absolute
 *        deviation from their median.
 * @param r The clean values (m).
 * @return The scale (m); 0 for an empty sample.
 */
double gfArcJumpRobustScale(const std::deque<double>& r);

/**
 * @brief Advance one arc by one GF sample and test the step.  Pure: the arc state is
 *        the only thing it changes.
 *
 * @param arc        The arc's state; updated in place.
 * @param epoch_idx  Monotonic processed-epoch index of this sample.
 * @param t_s        Time of this sample (continuous GPS seconds).
 * @param sig1       Primary-pair signal 1 the GF was formed on.
 * @param sig2       Primary-pair signal 2.
 * @param gf_m       GF = lambda1 L1 - lambda2 L2 (m).
 * @param p          Knob snapshot.
 * @return The verdict for this sample.
 */
GfArcJumpVerdict gfArcJumpStep(GfArcJumpArc& arc, long long epoch_idx, double t_s,
                               const std::string& sig1, const std::string& sig2,
                               double gf_m, const GfArcJumpParams& p);

/// Run-end counters of the trigger (standing invariant 4: armed with @ref tests at 0
/// is dead wiring, not a quiet run).  The flag closure:
///   flags == flags_freed + flags_coincident + flags_cb_kept + flags_no_arc
///            + flags_cold + flags_pivot.
struct GfArcJumpCounters {
    long long samples{0};          ///< Dual-band (station, satellite) rows seen.
    long long tests{0};            ///< Steps tested against a clean reference (the opportunities).
    long long flags{0};            ///< Tests flagged.
    long long flags_freed{0};      ///< Flags that freed at least one band (warm carry).
    long long flags_coincident{0}; ///< Flags whose matched bands were all freed earlier this epoch by another drain (cause bit added only).
    long long flags_cb_kept{0};    ///< Flags whose matched bands the common-band verdict all refused.
    long long flags_no_arc{0};     ///< Warm flags that matched no live ambiguity of the pair.
    long long flags_cold{0};       ///< Flags with the carry cold: diagnostic only.
    long long flags_pivot{0};      ///< Flags at a gauge-pinned station index below i_lo: never refloated locally.
    long long bands_freed{0};      ///< Ambiguities freed.
    long long bands_coincident{0}; ///< Ambiguities already freed this epoch that took the cause bit.
    long long bands_cb_kept{0};    ///< Ambiguities the common-band verdict kept pinned.
    long long scale_tests{0};      ///< Tests on which the scale term was active.
    long long scale_bound{0};      ///< ...of those, where k times the scale exceeded the floor (the scale set the threshold).
    long long restarts{0};         ///< Arc restarts (gap, pair change, span change).
    long long reseeds{0};          ///< References re-seeded after a full flag run.
    double    max_abs_r_m{0.0};    ///< Largest |r| over all tests (m).
    std::string max_abs_r_id;      ///< "<station>|<satellite>@<tow>|el=<deg>" of it.
    double    max_clean_r_m{0.0};  ///< Largest |r| over the unflagged tests (m).
    std::string max_clean_r_id;    ///< Its identity, as above.
};

/**
 * @brief The PASS-A drain: test every present station's dual-band arcs this epoch and
 *        answer each flag (file banner, "The response").
 *
 * Sited after the IF-TD driver and before the wind-up, gap re-entry and injected-free
 * drains and the lane observatory's PASS-A half, so a GF-freed pair enters this epoch's
 * free-epoch latch before anything reads it.  A no-op unless the knob is armed and the
 * raw observables were computed this epoch.  One event row per flag
 * (network_events.csv, tier @c T1_gf_jump, event @c gf_jump, statistic |r|, threshold
 * the bar applied).
 *
 * @param qc                 The slip-QC channel: knobs, raw rows, per-arc state, counters;
 *                           every flagged line of sight is also recorded in
 *                           @c qc.epoch.gf_jump_flagged (whatever the flag's action).
 * @param stations           The network's stations (StateManager and name read; marks written).
 * @param present            Per-station PASS-A presence.
 * @param i_lo               First locally refloatable station index (1 on the legacy pivot datum).
 * @param n_sta_total        Station count.
 * @param carry_invariant    The Card-4 DD vehicle is on: feed its funnel.
 * @param carry_warm         The leveling carry is warm: a flag acts (otherwise diagnostic).
 * @param carry_slipped_ambs The Card-4 funnel (inserted into under @p carry_invariant).
 * @param se                 This epoch (time).
 * @param traces             The trace bundle (event rows).
 * @param log                The main_log logger.
 * @param relevel            The free-epoch latch; null when relevel_free_epoch is off.
 * @param relevel_pend       Bridge pends to void on a fresh free; may be null.
 * @param common_band        This epoch's common-band verdicts; null when the scope is off.
 */
void networkSlipDrainGfArcJump(SlipQcChannel& qc,
                               std::vector<NetStation>& stations,
                               const std::vector<bool>& present,
                               int i_lo, int n_sta_total,
                               bool carry_invariant, bool carry_warm,
                               std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                               const SyncedEpoch& se, NetworkRunTraces& traces,
                               const std::shared_ptr<spdlog::logger>& log,
                               RelevelEpochState* relevel,
                               std::map<std::tuple<int, std::string, std::string>,
                                        RelevelPend>* relevel_pend,
                               const CommonBandEpoch* common_band);

/**
 * @brief Run-end report: the arc-scoped GF jump trigger.
 *
 * The counters with the flag closure, the largest |r| tested and the largest unflagged
 * one with their identities, and the stations with the most flags.
 *
 * Gate: @c network.cycle_slip.gf_arc_jump (returns 0 at once when off).
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkGfArcJumpRunEnd(const NetworkRunEndView& v);

} // namespace GnssProcessor
