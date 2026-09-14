#pragma once
/**
 * @file NetworkGrossLos.hpp
 * @brief Card GL — the GROSS LINE-OF-SIGHT screen for the PPP-RTK network
 *        producer (docs/SPEC_event_harness.md §24.18).
 *
 * ## The blind spot this closes
 *
 * Every post-fit tier the producer owns screens CARRIED-AMBIGUITY PHASE rows and
 * nothing else.  The admission test is one line — `NetworkRaim.cpp:328`:
 *
 * @code
 *   if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
 * @endcode
 *
 * An uncombined phase row carries @f$+1@f$ on its ambiguity column; a CODE row
 * carries exactly zero on every ambiguity column.  So a code row can never enter
 * the σ̂₀ pool, never contribute to a cell's @c r_max, never appear in the
 * median-polish table, and can never reach the cross-station consensus or the
 * isolated-arc attribution.  A gross CODE error on one line of sight is therefore
 * structurally invisible to post-fit RAIM — not rare, not under-weighted,
 * *unreachable*.
 *
 * The one tier that DOES visit the row is the MDB rider, and its statistic is a
 * RELIABILITY number, @f$\delta_0\sigma_i/\sqrt{r_i}@f$ — "how large a fault
 * could hide here" — which never reads @f$v_i@f$ at all.  Its action column is the
 * string literal @c "observe" (`NetworkMdb.cpp:286`) and its own header says
 * "nothing downstream reads @c mdb_event_m".
 *
 * Measured consequence (§24.15 R5, §24.17): on `runs/_cm_pvb` the line of sight
 * (DJIG00DJI, G06) ran post-fit code residuals of −75 … −142 m on three signals
 * for 31 epochs; `network_raim.csv` filed ZERO rows for that station or satellite
 * in the window, `n_adapt` was 0, and MDB observed twice.  Over six hours such
 * lines are admitted at birth, never acted on, and grow until a whole station is
 * poisoned — BSHM00ISR at hour 5 of `runs/_cm_c10`: station code rms 54 m, ZWD
 * 197 m.
 *
 * ## What the screen is
 *
 * A per (station, satellite) LINE-OF-SIGHT state machine over the post-fit CODE
 * residuals, with an EVENT at each edge (standing invariant #7) and run-end
 * counters on every denominator (standing invariant #4):
 *
 *  - **Statistic.**  @f$r_i = |v_i|\sqrt{w_i}@f$ with @f$w_i = 1/\sigma_i^2@f$ —
 *    each code row standardized by its OWN σ, so the bar @ref
 *    NetworkConfig::gross_los_k reads literally as "this observation missed by k
 *    of its own sigmas".  The pooled robust σ̂₀ is deliberately NOT the
 *    denominator: on the hour a station is being poisoned the pool inflates, and
 *    a screen normalized by it would disarm itself exactly when it is needed.
 *    (That is the same property that makes RAIM's σ̂₀ the right choice for
 *    ambient PHASE misfit and the wrong one here.)
 *  - **Declaration.**  At least @ref NetworkConfig::gross_los_min_rows of the
 *    LOS's own code rows over the bar in the same epoch, for @ref
 *    NetworkConfig::gross_los_epochs CONSECUTIVE epochs.  A gross line of sight
 *    is wrong on every code band at once (a range error, not a dispersive one);
 *    one heavy-tailed row is one band's multipath.  Absence resets the streak —
 *    an unobserved epoch is not evidence either way, the same rule RAIM's
 *    isolated-arc persistence gate uses.
 *  - **Response (the DECOUPLING edge).**  Exactly the refloat vocabulary the T4
 *    attribution erase already owns, so the two responses cannot drift: erase the
 *    LOS's entries in @c ctx.carry.local_carry (its zero-q phase levels), erase
 *    @c ctx.carry.iono_carry / @c iono_blk_carry for the LOS (so its slant iono
 *    re-draws from the SH tie rather than from a value estimated jointly with a
 *    level that is no longer trusted), mark its ambiguities pending-reseed, and
 *    from the NEXT assembly fold its CODE rows at σ × @ref
 *    NetworkConfig::gross_los_deweight.  A CAP, not a drop (prefer-fallback): the
 *    row keeps its sign and still re-anchors the float if the LOS's phase side
 *    goes away.  PHASE rows are untouched — they have their own responses.
 *  - **Release (the RE-COUPLING edge).**  The LOS's worst code row under @ref
 *    NetworkConfig::gross_los_clear_sigma for @ref
 *    NetworkConfig::gross_los_clear_epochs consecutive epochs lifts the
 *    deweight, with its own event.  The gap between the two bars is hysteresis;
 *    the loader warns if it is absent.
 *  - **Expiry (invariant #1's absent-satellite edge).**  A declared LOS unseen
 *    for longer than @ref NetworkConfig::gross_los_max_gap_s expires with its own
 *    event.  The next sighting is a new arc with a new ambiguity and the old
 *    evidence cannot speak for it; without this a set-and-forget deweight would
 *    ride a fresh arc silently.
 *
 * **The epoch always EMITS.**  Nothing here withholds, drops a station or drops a
 * satellite — this is adapt-and-emit, and a withhold would be graded as an
 * adaptation failure.
 *
 * ## Contract
 *
 * Default-OFF; OFF the seam is not called, no state is allocated, the assembly's
 * code-scale map is never populated and the run is BYTE-IDENTICAL (standing
 * invariant #6).  NOT fingerprinted: the knob does not change what any carried
 * state MEANS, only which rows are trusted this epoch, so a snapshot written with
 * it off stays readable with it on.
 *
 * Seam contract per CLAUDE.md: a free function over the run context, owning NO
 * state — the streaks, the declared set and every counter are references into
 * @c NetworkRunContext.  Ordering is the RAIM one and for the same reason: it
 * MUST run AFTER the joint solve and the carry harvest (erasing a @c local_carry
 * entry IS the refloat, and an entry erased before the harvest would simply be
 * re-stashed), and it is placed immediately after @ref networkPostFitRaim inside
 * the caller's @c SB_RAIM bracket.  The erases are idempotent, so a line of sight
 * that RAIM already refloated this epoch is not harmed by a second visit.
 */

#include "filter/NetworkEstimator.hpp"   // NetworkEstimator (reconstructStation / labels)
#include "filter/NetworkRunTypes.hpp"    // NetStation / StationAssembly
#include "rinex/MultiRinexSync.hpp"      // SyncedEpoch

#include <Eigen/Core>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Run-end telemetry for the gross-LOS screen (standing invariant #4).
 *
 * The DENOMINATORS are the point, exactly as for @c NetworkRaimCiTallies: they
 * make four states distinguishable that all otherwise read as "the screen found
 * nothing".
 *
 *  - @c epochs == 0                        → the seam never ran (call-site gate).
 *  - @c epochs > 0, @c los_screened == 0   → it ran and the census was EMPTY (no
 *                                            code row reached it — dead wiring).
 *  - @c los_screened > 0, @c los_over == 0 → it screened real residuals and
 *                                            nothing cleared the bar (zero
 *                                            opportunity, not zero capability).
 *  - @c los_over > 0, @c declared == 0     → evidence existed but never persisted
 *                                            @c gross_los_epochs epochs.
 *
 * Every member stays at zero unless @c ctx.knobs.gl_on, so an OFF run allocates
 * nothing and reports nothing.
 */
struct NetworkGrossLosTallies {
    long long epochs = 0;            ///< Seam passes with the screen armed.
    long long stations_screened = 0; ///< Station-epochs that produced at least one code row.
    long long los_screened = 0;      ///< (station, satellite) LOS-epochs screened.
    long long rows_screened = 0;     ///< CODE rows visited.
    long long los_over = 0;          ///< LOS-epochs whose evidence cleared the bar.
    long long declared = 0;          ///< Declarations (decoupling edges).
    long long cleared = 0;           ///< Releases (re-coupling edges).
    long long expired = 0;           ///< Declarations retired on the absent-satellite edge.
    long long suppressed_warmup = 0; ///< LOS-epochs whose evidence was complete but the MATURITY horizon held the declaration.
    long long suppressed_station = 0;///< LOS-epochs whose evidence was complete but the WHOLE-STATION stand-down held the declaration.
    long long standdowns = 0;        ///< Station-epochs the stand-down fired on.
    std::map<std::string, long long> standdown_by_station;  ///< Stand-down station-epochs per station name.
    long long currently_gross = 0;   ///< Declared and not yet cleared, at run end.
    long long max_concurrent = 0;    ///< Largest simultaneous declared set over the run.
    long long rows_deweighted = 0;   ///< CODE rows actually folded at the deweighted σ (counted in the assembly).
    long long amb_refloated = 0;     ///< Carried ambiguity levels erased + marked pending-reseed.
    long long iono_erased = 0;       ///< Per-LOS iono carry entries erased (either vehicle).
    double    r_max = 0.0;           ///< Largest standardized code residual any screened LOS reached.
    double    v_abs_max_m = 0.0;     ///< Largest |v| (metres) any screened LOS reached.
    std::map<std::string, long long> declared_by_station;   ///< Declarations per station name.
    std::map<std::string, long long> declared_by_sat;       ///< Declarations per satellite.
};

/**
 * @brief PURE: restore a folded code row's NOMINAL standardized residual.
 *
 * ⚠ The one-liner that stops the response from laundering its own release test.
 *
 * A declared line of sight's code rows are folded at @f$\sigma \times f@f$, i.e.
 * @f$w \to w/f^2@f$, so the measured @f$|v|\sqrt{w}@f$ reads @f$f@f$ times
 * SMALLER than the same residual at nominal weight.  With the default @f$f=100@f$
 * a 144 m residual reads 1.3 σ — comfortably under the 3 σ release bar, one epoch
 * after the very same line of sight was declared at 125 σ.
 *
 * MEASURED, not hypothesised (the first `runs/_gl_on6`, DJIG00DJI/G06):
 * declare TOW 265500 at 125.6 σ with |v| 143.8 m → **clear** 265560 at 1.28 σ
 * with |v| still **144.7 m** → declare 265620 → clear 265680.  A two-epoch limit
 * cycle in which the deweight is on half the time and the line of sight's carried
 * level is erased for ever.
 *
 * The undo is exact because the fold is exact and happens in exactly one place
 * (`Application::networkSplitStation_`), with the factor taken from the same
 * @c NetStation::gl_code_sigma_scale map this seam reads before republishing it.
 *
 * @param r_folded @f$|v|\sqrt{w}@f$ as measured, with the fold already in @c w.
 * @param scale    The σ multiplier the assembly applied to this row (1 = none).
 * @return the statistic the row would have had at its nominal σ.
 */
double networkGrossLosUndoDeweight(double r_folded, double scale);

/**
 * @brief One CODE row of a line of sight, as the pure decision rule sees it.
 */
struct GrossLosSample {
    std::string signal;      ///< Signal label from the row's @c CB: / @c PB: column; EMPTY when the posture gives the row no such column (external biases, or the W6 datum signal).
    double      r{0.0};      ///< @f$|v|\sqrt{w}@f$ — the residual in units of the row's OWN σ.
    double      v_m{0.0};    ///< The SIGNED residual, metres (event detail only).
};

/**
 * @brief The per-epoch verdict on ONE line of sight.  @see networkGrossLosAssess
 */
struct GrossLosVerdict {
    bool        over{false};     ///< The epoch's evidence clears the declaration bar.
    bool        under_clear{false}; ///< Every code row sits under the clear bar (the release evidence).
    int         n_rows{0};       ///< Code rows examined.
    int         n_over{0};       ///< Code rows above @c k.
    double      r_max{0.0};      ///< Largest standardized residual on the LOS.
    double      v_abs_max{0.0};  ///< Largest |v| (metres) on the LOS.
    std::string signals;         ///< ';'-joined DISTINCT non-empty signal labels of the rows over @c k (event detail).
};

/**
 * @brief PURE: adjudicate one line of sight's code rows for THIS epoch.
 *
 * The rule is deliberately a free function so it is unit-testable without a
 * network, following the @c networkRaimEraseBandScope precedent.
 *
 * Two deliberate edges:
 *  - **Counted as ROWS, not distinct signal labels.**  A code row of a posture
 *    whose satellite code biases are supplied externally, and the W6 datum
 *    signal's rows even when they are estimated, carry no @c CB: column and so
 *    have an EMPTY signal label; a distinct-label test would silently under-count
 *    exactly there and the screen would be inert on the postures that need it
 *    most.  The distinct labels that were over the bar still reach the event.
 *  - **@c under_clear is NOT `!over`.**  The release bar is far below the
 *    declaration bar, and the band between them is neither evidence to declare
 *    nor evidence to release — a line of sight sitting there keeps its current
 *    disposition, which is what makes the hysteresis real rather than nominal.
 *    An empty row set is @c under_clear = false for the same reason: absence is
 *    not evidence of health (standing invariant #1).
 *
 * @param rows        This LOS's code rows this epoch (may be empty).
 * @param k           Declaration bar, in units of the row's own σ.
 * @param min_rows    Rows that must clear @p k together (≥ 1).
 * @param clear_sigma Release bar, same units.
 * @return The verdict.
 */
GrossLosVerdict networkGrossLosAssess(const std::vector<GrossLosSample>& rows,
                                      double k, int min_rows, double clear_sigma);

/**
 * @brief The persistent per-LOS disposition carried across epochs.
 */
struct GrossLosState {
    int    over_streak{0};      ///< Consecutive epochs of declaration evidence (reset by absence).
    int    clear_streak{0};     ///< Consecutive epochs of release evidence while declared.
    bool   gross{false};        ///< Currently declared (its code rows are deweighted).
    double declared_tow{0.0};   ///< TOW of the declaration that is currently standing.
    double last_seen_s{0.0};    ///< Continuous seconds stamp of the last epoch this LOS produced a code row.
    long long n_declared{0};    ///< Declarations over the run for this LOS (an episode counter, not a flag).
};

/// The edge a @ref networkGrossLosStep transition produced.
enum class GrossLosEdge {
    None,       ///< No edge this epoch.
    Declare,    ///< Decoupling edge: the LOS became gross.
    Clear,      ///< Re-coupling edge: the LOS was released.
    Suppressed  ///< The evidence was COMPLETE and a guard held the declaration (never silent: it has its own counters).
};

/**
 * @brief PURE: advance one line of sight's state machine by one SCREENED epoch.
 *
 * Only called for an epoch in which the LOS produced code rows; absence is
 * handled by the caller (the streak reset and, past @c gross_los_max_gap_s, the
 * expiry edge), because "unobserved" is not a transition of this machine.
 *
 * A CLEAR is never blocked by @p allow_declare.  Releasing a line of sight the data
 * says is healthy is always safe, and a guard that also froze the release edge would
 * strand a response with no way out — standing invariant #7's re-coupling edge
 * failing CLOSED instead of open.
 *
 * When @p allow_declare is false the completed streak is HELD at @p declare_epochs
 * rather than discarded or allowed to grow: the evidence is real, so throwing it away
 * would make the guard a memory hole, and letting it accumulate would make the gate's
 * opening a burst of simultaneous declarations across every line of sight that
 * waited.  Held, the line of sight declares on the first qualifying epoch after the
 * gate opens and on no earlier one.
 *
 * @param s             [in/out] the LOS's carried state.
 * @param v             This epoch's verdict.
 * @param declare_epochs Consecutive over-bar epochs required (≥ 1).
 * @param clear_epochs   Consecutive under-bar epochs required (≥ 1).
 * @param tow           This epoch's TOW (stamps a declaration).
 * @param allow_declare False when a guard forbids a NEW declaration this epoch — the
 *                      MATURITY horizon (@c network.gross_los_warmup_epochs) or the
 *                      WHOLE-STATION stand-down (@c network.gross_los_max_station_frac).
 * @return the edge, if any; @c GrossLosEdge::Suppressed when a guard held a
 *         COMPLETE declaration (a population with its own run-end counter, so a
 *         guard is never silent — standing invariant #4).
 */
GrossLosEdge networkGrossLosStep(GrossLosState& s, const GrossLosVerdict& v,
                                 int declare_epochs, int clear_epochs, double tow,
                                 bool allow_declare = true);

/**
 * @brief Run the gross-LOS screen for this epoch (the seam).
 *
 * Walks each station in the solve once, forms @f$v = y - A_s x_s - A_r x_r@f$,
 * classifies every DATA row with the SAME family rule @c networkPostfitSigma0
 * uses (SH-touching rows excluded, coupling rows excluded, a row touching an
 * ambiguity column or a @c PB: column is PHASE, a remaining row touching a
 * @c CB: or @c CLK: column is CODE), bins the code rows per satellite, and drives
 * the state machine above.
 *
 * ⚠ MUTATES persistent carries.  It erases @c ctx.carry.local_carry entries and
 * marks StateManager ambiguities pending-reseed for every declared LOS, erases
 * @c ctx.carry.iono_carry / @c iono_blk_carry for it, and rewrites
 * @c ctx.carry.gl_gross / @c gl_state, which the NEXT epoch's assembly reads
 * through @c NetStation::gl_code_sigma_scale.  It therefore MUST run AFTER the
 * carry harvest and BEFORE the epoch stash tail — the caller places it
 * immediately after @ref networkPostFitRaim inside the @c SB_RAIM bracket.
 *
 * No-op (not even a state allocation) unless @c ctx.knobs.gl_on.
 *
 * @param ctx           [in/out] knobs, carry (streaks + declared set + the two
 *                      carry maps it erases from) and counters.
 * @param config_       Live config — the @c network.gross_los_* block.
 * @param log           "main_log": one summary line per epoch with any edge.
 * @param traces        GROSSLOS-tier events (both edges + expiry).
 * @param se            This epoch (week/TOW stamp).
 * @param stations      [in/out] names key the events; @c sm->markAmbiguityPendingReseed
 *                      on a declaration.
 * @param est           Estimator (reconstructStation / sharedColumnLabel; both const).
 * @param x_s           Post-fit shared solution vector.
 * @param asm_res       Per-station assembly (A_s / A_r / y / w / pl_amb / qc_amb).
 * @param est_index_of  Station → reduced-system index (< 0 = absent from the solve).
 * @param now_s         Continuous seconds (week·604800 + TOW) for the gap edge.
 */
void networkGrossLosScreen(NetworkRunContext& ctx,
                           const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           NetworkRunTraces& traces,
                           const SyncedEpoch& se,
                           std::vector<NetStation>& stations,
                           const NetworkEstimator& est,
                           const Eigen::VectorXd& x_s,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           double now_s);

/**
 * @brief Republish each station's CODE-row σ scale from the declared set.
 *
 * The deweight actuator is the exact CODE counterpart of the signal-integrity
 * card's @c NetStation::phase_sigma_scale, and it obeys the same thread-safety
 * contract: this function runs SERIALLY, before the parallel PASS-B produce, and
 * the assembly only find-reads the map.  An empty map is the OFF path — the
 * assembly's guard is `if (!st.gl_code_sigma_scale.empty())`, so an unarmed run
 * performs the exact @c cwiseInverse weights with zero extra floating-point
 * operations.
 *
 * Idempotent, and cheap: it clears and rebuilds from @c ctx.carry.gl_state, whose
 * declared subset is O(number of currently gross lines of sight).
 *
 * @param ctx      Run context (reads @c carry.gl_state and @c knobs).
 * @param stations [in/out] each station's @c gl_code_sigma_scale.
 */
void networkGrossLosPublishScales(NetworkRunContext& ctx,
                                  std::vector<NetStation>& stations);

} // namespace GnssProcessor
