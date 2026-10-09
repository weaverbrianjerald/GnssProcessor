#pragma once
/**
 * @file NetworkGrossLos.hpp
 * @brief Gross line-of-sight screen for the PPP-RTK network producer (card GL,
 *        docs/SPEC_event_harness.md §24.18).
 *
 * ## Coverage gap addressed
 *
 * Every other post-fit tier of the producer screens carried-ambiguity phase rows
 * only.  The admission rule of @c networkPostFitRaim (NetworkRaim.cpp) is the declared
 * one of filter/NetworkRowClassify.hpp (card NOM step N2b): a row enters the pool of an
 * ambiguity column when its coefficient on the column exceeds 0.5 and its kind is one
 * @c raimPoolAdmitsKind admits (the observation phase rows, the coupling and block
 * carried-prior rows, and the SD-EWL constraint rows).  No code kind is admitted.
 *
 * An uncombined phase row carries @f$+1@f$ on its ambiguity column; a code row
 * carries zero on every ambiguity column and is of a kind the rule does not admit.  A
 * code row therefore never enters the
 * σ̂₀ pool, never contributes to a cell's @c r_max, never appears in the
 * median-polish table, and never reaches the cross-station consensus or the
 * isolated-arc attribution.  A gross code error on one line of sight is out of
 * reach of post-fit RAIM altogether, not merely under-weighted.
 *
 * The one tier that visits the row is the MDB rider, and its statistic is a
 * reliability number, @f$\delta_0\sigma_i/\sqrt{r_i}@f$ (the largest fault that
 * could stay hidden there), which never reads @f$v_i@f$.  Its action column is
 * the string literal @c "observe" (NetworkMdb.cpp), and NetworkMdb.hpp states
 * that no decision reads its @c mdb_event_m threshold.
 *
 * Field cases (§24.15 R5, §24.17): without this screen, a line of sight with
 * post-fit code residuals of tens to hundreds of metres on every signal is
 * admitted, never acted on, and can grow until the whole station's solution
 * degrades.
 *
 * ## The screen
 *
 * A per (station, satellite) line-of-sight state machine over the post-fit code
 * residuals, with an event at each edge (standing invariant #7) and run-end
 * counters on every denominator (standing invariant #4):
 *
 *  - **Statistic.**  @f$r_i = |v_i|\sqrt{w_i}@f$ with @f$w_i = 1/\sigma_i^2@f$:
 *    each code row is standardized by its own σ, so the bar @ref
 *    NetworkConfig::gross_los_k means "this observation missed by k of its own
 *    sigmas".  The pooled robust σ̂₀ is not the denominator: while a station is
 *    degrading, the pool inflates, and a screen normalized by it would lose
 *    sensitivity when it is needed.  (The same property makes RAIM's σ̂₀ the right
 *    normalization for ambient phase misfit and the wrong one here.)
 *  - **Declaration.**  At least @ref NetworkConfig::gross_los_min_rows of the
 *    LOS's own code rows over the bar in the same epoch (under
 *    @c network.gross_los_min_rows_rule = available, at least
 *    min(gross_los_min_rows, the LOS's code rows)), for @ref
 *    NetworkConfig::gross_los_epochs consecutive epochs.  A gross line of sight is
 *    wrong on every code band at once (a range error, not a dispersive one); a
 *    single heavy-tailed row is one band's multipath.  Absence resets the streak:
 *    an unobserved epoch is not evidence either way, the same rule RAIM's
 *    isolated-arc persistence gate uses.
 *  - **Response (the decoupling edge).**  The refloat vocabulary the T4
 *    attribution erase already uses, so the two responses stay consistent: erase
 *    the LOS's entries in @c ctx.carry.local_carry (its zero-q phase levels),
 *    erase @c ctx.carry.iono_carry / @c iono_blk_carry for the LOS (so its slant
 *    iono re-draws from the SH tie rather than from a value estimated jointly with
 *    a level that is no longer trusted), and mark its ambiguities pending-reseed.
 *    Under @c network.gross_los_bound those entries are held at their
 *    pre-declaration values instead of erased (see @ref networkGrossLosAct).
 *    From the next assembly on, the LOS's code rows fold at σ × @ref
 *    NetworkConfig::gross_los_deweight.  This is a cap, not a drop
 *    (prefer-fallback): the row keeps its sign and still re-anchors the float if
 *    the LOS's phase side goes away.  Phase rows are untouched; they have their
 *    own responses.
 *  - **Release (the re-coupling edge).**  The LOS's worst code row under @ref
 *    NetworkConfig::gross_los_clear_sigma for @ref
 *    NetworkConfig::gross_los_clear_epochs consecutive epochs lifts the deweight,
 *    with its own event.  The gap between the two bars is the hysteresis; the
 *    loader warns if it is absent.
 *  - **Expiry (invariant #1's absent-satellite edge).**  A declared LOS unseen
 *    for longer than @ref NetworkConfig::gross_los_max_gap_s expires with its own
 *    event.  The next sighting is a new arc with a new ambiguity, and the old
 *    evidence does not apply to it; without the expiry, a deweight set once would
 *    carry over to a fresh arc unnoticed.
 *
 * ## The cold-start bound (card ICF response 1, @c network.gross_los_cold_bound_m)
 *
 * The maturity horizon holds every σ-bar declaration for the first
 * @c gross_los_warmup_epochs emitted epochs, because a cold post-fit code residual
 * measures the unconverged station block.  A faulty receiver channel inside that window
 * is not that: on the field golden, BSHM00ISR tracks G07 on a faulty L1 channel (C1C
 * and L1C drift together), and once its carried ambiguity makes the line of sight
 * redundant it puts about -2.2 m per epoch into G07's clock, which the dense border
 * carry keeps (card ICF section 6).  Its post-fit code |v| is 24 m at its first
 * visible epoch, against a largest clean value of 6.7 m (field) and 4.9 m (sim) over
 * the first 120 epochs of the 2 h goldens.
 *
 *  - **Declaration.**  Inside the horizon, a line of sight whose largest post-fit code
 *    |v| (metres, @ref GrossLosVerdict::v_abs_max) exceeds the bound is declared at
 *    once: one epoch, no row count, no persistence.  The response is the screen's own
 *    (the erase, pending-reseed and code fold below); @ref GrossLosState::cold records
 *    which path declared it.  A station with more than
 *    @c gross_los_max_station_frac of its lines of sight over the bound declares none
 *    of them (a station fault, as for the σ bar), with its own event.
 *  - **The redundancy taken away.**  With the slant ionosphere free, a single-band line
 *    of sight is redundant only through its carried ambiguity (code plus phase), and the
 *    code fold weakens that sum by @f$f^2@f$.  A two-band line of sight is also redundant
 *    through its phase rows alone, which the code fold does not reach.  So while a cold
 *    declaration stands, every epoch on which the bound is exceeded again erases the
 *    carried ambiguity again: a free ambiguity absorbs its phase row exactly, whatever
 *    the number of bands.
 *  - **Release (the re-coupling edge).**  The screen's own release test.  At the release
 *    (and at an expiry) the carried ambiguity is erased once more, so the line of sight
 *    reconnects on a fresh ambiguity, not on one harvested while the fault was in its
 *    rows (standing invariant #7: the re-coupling edge re-seeds what it reconnects).
 *
 * The cold path's edges are counted apart from the σ-bar path's (@c cold_* in
 * @ref NetworkGrossLosTallies) and reported by @ref networkGrossLosColdBoundRunEnd.
 * Off (bound 0, the default), nothing on this path runs and the run is byte-identical.
 *
 * The epoch always emits: this is adapt-and-emit, and an epoch withhold would be
 * graded as an adaptation failure.  The per-LOS screen never drops a station or a
 * satellite.  Two optional sub-rules prune individual satellites from an
 * emission while every other satellite still emits: the GL-2 wire withhold
 * (@c network.gross_los_sat_wire_withhold) and the RF-3b B3 emission exclusion
 * (@c network.gross_los_emit_exclude).
 *
 * ## Contract
 *
 * Off by default.  With it off, the seam returns on entry, no state is allocated,
 * the assembly's code-scale map is never populated, and the run is
 * byte-identical (standing invariant #6).  Not fingerprinted: the knob changes
 * which rows are trusted this epoch, not what any carried state means, so a
 * snapshot written with it off stays readable with it on.
 *
 * Seam contract per CLAUDE.md: a free function over the run context that owns no
 * state; the streaks, the declared set and every counter are references into
 * @c NetworkRunContext.  The response half must run after the joint solve and
 * the carry harvest, for the same reason as RAIM: erasing a @c local_carry entry
 * is the refloat, and an entry erased before the harvest would be re-stashed.  The
 * caller places that call immediately after @ref networkPostFitRaim inside its
 * @c SB_RAIM bracket.  The erases are idempotent, so a second visit to a line of
 * sight that RAIM already refloated this epoch has no further effect.
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/**
 * @brief Run-end telemetry for the gross-LOS screen (standing invariant #4).
 *
 * The denominators matter, as for @c NetworkRaimCiTallies: they separate four
 * states that would otherwise all read as "the screen found nothing".
 *
 *  - @c epochs == 0                        → the seam never ran (call-site gate).
 *  - @c epochs > 0, @c los_screened == 0   → it ran and the census was empty (no
 *                                            code row reached it: dead wiring).
 *  - @c los_screened > 0, @c los_over == 0 → it screened real residuals and
 *                                            nothing cleared the bar (no
 *                                            opportunity, not no capability).
 *  - @c los_over > 0, @c declared == 0     → evidence existed but never persisted
 *                                            for @c gross_los_epochs epochs.
 *
 * Every member stays at zero unless @c ctx.knobs.gl_on, so an unarmed run
 * allocates nothing and reports nothing.
 */
struct NetworkGrossLosTallies {
    long long epochs = 0;            ///< Seam passes with the screen armed.
    long long stations_screened = 0; ///< Station-epochs that produced at least one code row.
    long long los_screened = 0;      ///< (station, satellite) LOS-epochs screened.
    long long rows_screened = 0;     ///< Code rows visited.
    long long los_over = 0;          ///< LOS-epochs whose evidence cleared the bar.
    long long declared = 0;          ///< Declarations (decoupling edges).
    long long cleared = 0;           ///< Releases (re-coupling edges).
    long long expired = 0;           ///< Declarations retired on the absent-satellite edge.
    long long suppressed_warmup = 0; ///< LOS-epochs with complete evidence whose declaration the maturity horizon held.
    long long suppressed_station = 0;///< LOS-epochs with complete evidence whose declaration the whole-station stand-down, or a standing station-level declaration (RF-3b B4), held.
    long long standdowns = 0;        ///< Station-epochs the stand-down fired on.
    std::map<std::string, long long> standdown_by_station;  ///< Stand-down station-epochs per station name.
    // --- Satellite stand-down (GL-2, §24.19); all zero unless network.gross_los_sat_standdown
    long long sat_standdowns = 0;      ///< Satellite stand-down edges (decoupling).
    long long sat_released = 0;        ///< Satellite release edges (re-coupling).
    long long sat_epochs = 0;          ///< Epochs with at least one satellite stood down.
    long long sat_epoch_max = 0;       ///< Most satellites stood down at the same time.
    long long sat_los_suppressed = 0;  ///< Per-LOS declarations refused because their satellite was stood down.
    long long sat_los_released = 0;    ///< Standing per-LOS declarations released because their satellite was stood down.
    long long sat_wire_withheld = 0;   ///< Satellite-epochs actually pruned from an emission (counted at the prune, in NetworkSolPrep).
    std::map<std::string, long long> sat_standdown_by_sat;  ///< Stand-down edges per satellite.
    long long currently_gross = 0;   ///< Declared and not yet cleared, at run end.
    long long max_concurrent = 0;    ///< Largest simultaneous declared set over the run.
    long long rows_deweighted = 0;   ///< Code rows actually folded at the deweighted σ (counted in the assembly).
    long long amb_refloated = 0;     ///< Ambiguity columns the declaration response reached: erased and marked pending-reseed, or held under @c network.gross_los_bound.
    long long iono_erased = 0;       ///< Per-LOS iono carry entries erased (either vehicle).
    double    r_max = 0.0;           ///< Largest standardized code residual any screened LOS reached.
    double    v_abs_max_m = 0.0;     ///< Largest |v| (metres) any screened LOS reached.
    std::map<std::string, long long> declared_by_station;   ///< Declarations per station name.
    std::map<std::string, long long> declared_by_sat;       ///< Declarations per satellite.
    // --- Counters of the RF-3b sub-rules (docs/refound/RF-3_code_row_qc_2026-09-22.md
    //     §4 B5, requirement F6).
    //     The B5 counters (los_over_short, folded_v_abs_max_m, the latencies) are kept
    //     whether or not the sub-rule they describe is armed, so an arm that leaves a rule
    //     off still reports the population that rule would have reached.  The B2, B3 and
    //     B4 counters stay zero unless their own knob is armed.
    long long los_over_short = 0;    ///< Population of B1: LOS-epochs whose evidence clears the bar under the available rule while the LOS has fewer code rows than @c gross_los_min_rows, which the fixed rule cannot reach.  Counted under either rule.
    long long bound_los_epochs = 0;  ///< LOS-epochs on which a hold was re-applied over the carry (B2; 0 unless @c network.gross_los_bound).
    long long bound_amb_held = 0;    ///< Carried ambiguity entries frozen at a declaration (B2).
    long long bound_iono_held = 0;   ///< Per-LOS iono carry entries frozen at a declaration (B2).
    double    folded_v_abs_max_m = 0.0;  ///< Largest |v| (m) reached by a row that was actually folded at the deweight (B5, the requirement F2 alarm).  0 when nothing was ever folded.
    long long declare_latency_sum_ep = 0;///< Summed per-episode latency, in over-bar screened epochs, from the LOS's first over-bar epoch to its declaration (B5).
    long long declare_latency_max_ep = 0;///< Largest such latency (B5).
    long long emit_excluded = 0;     ///< Satellite-epochs with at least one declared line of sight (or declared station) among their contributors, counted every screened epoch the declaration stands (B3; 0 unless @c network.gross_los_emit_exclude).
    long long emit_withheld = 0;     ///< Satellite-epochs actually pruned from an emission because too few contributors remained (B3).
    long long emit_disc_marked = 0;  ///< Satellite-epochs emitted with a per-satellite discontinuity mark instead of being withheld (B3).
    long long sta_declared = 0;      ///< Station-level declarations (B4).
    long long sta_released = 0;      ///< Station-level releases (B4).
    long long sta_los_epochs = 0;    ///< Station-epochs spent under a standing station declaration (B4).
    std::map<std::string, long long> sta_declared_by_station;  ///< Station declarations per station name (B4).
    // --- The cold-start bound (card ICF response 1); all zero unless
    //     network.gross_los_cold_bound_m is positive.  The cold path's edges are counted
    //     here only, not in declared / cleared / expired above, so those keep describing
    //     the σ-bar path; currently_gross and max_concurrent count both paths.
    long long cold_los_screened = 0;     ///< LOS-epochs screened inside the maturity horizon (the denominator).
    long long cold_los_over = 0;         ///< LOS-epochs inside the horizon whose largest post-fit code |v| exceeded the bound.
    long long cold_declared = 0;         ///< Cold declarations (decoupling edges).
    long long cold_held_station = 0;     ///< LOS-epochs over the bound whose declaration the cold station guard held.
    long long cold_standdowns = 0;       ///< Station-epochs on which the cold station guard fired.
    long long cold_refloats = 0;         ///< Re-erases of a standing cold declaration whose bound was exceeded again.
    long long cold_refloats_after_hold = 0; ///< The subset of @ref cold_refloats taken after the maturity horizon opened.
    long long cold_released = 0;         ///< Releases of a cold declaration (re-coupling edges), each with a carry re-seed.
    long long cold_expired = 0;          ///< Expiries of a cold declaration, each with a carry re-seed.
    long long cold_amb_erased = 0;       ///< Carried ambiguity entries removed by the re-erases and the re-seeds.
    double    cold_v_over_max_m = 0.0;   ///< Largest post-fit code |v| (m) over the bound.
    double    cold_v_under_max_m = 0.0;  ///< Largest post-fit code |v| (m) under the bound inside the horizon, on a line of sight not declared: the run's clean-population maximum, i.e. the bound's margin.
    std::map<std::string, long long> cold_declared_by_los;  ///< Cold declarations per "STATION:SAT".
};

/**
 * @brief Pure function: restore a folded code row's nominal standardized residual.
 *
 * Without this correction the response would defeat its own release test.  A
 * declared line of sight's code rows are folded at @f$\sigma \times f@f$, i.e.
 * @f$w \to w/f^2@f$, so the measured @f$|v|\sqrt{w}@f$ reads @f$f@f$ times
 * smaller than the same residual at nominal weight.  With the default
 * @f$f=100@f$, a 144 m residual declared at 125 σ reads 1.3 σ one epoch later,
 * under the 3 σ release bar.  The line of sight then alternates between declare
 * and clear every other epoch: the deweight is on half the time, and its carried
 * level is erased at every declaration.
 *
 * The undo is exact because the fold is exact and happens in one place
 * (`Application::networkSplitStation_`), with the factor taken from the same
 * @c NetStation::gl_code_sigma_scale map this seam reads before republishing it.
 *
 * @param r_folded @f$|v|\sqrt{w}@f$ as measured, with the fold already in @c w.
 * @param scale    The σ multiplier the assembly applied to this row (1 = none).
 * @return the statistic the row would have had at its nominal σ.
 */
double networkGrossLosUndoDeweight(double r_folded, double scale);

/**
 * @brief One code row of a line of sight, as the pure decision rule sees it.
 */
struct GrossLosSample {
    std::string signal;      ///< The row's observation code, from its contribution tag (card NOM step N2b); empty when the tag names no signal.
    double      r{0.0};      ///< @f$|v|\sqrt{w}@f$: the residual in units of the row's own σ.
    double      v_m{0.0};    ///< The signed residual, metres (event detail only).
};

/**
 * @brief The per-epoch verdict on one line of sight.  @see networkGrossLosAssess
 */
struct GrossLosVerdict {
    bool        over{false};     ///< The epoch's evidence clears the declaration bar.
    bool        under_clear{false}; ///< Every code row sits under the clear bar (the release evidence).
    int         n_rows{0};       ///< Code rows examined.
    int         n_over{0};       ///< Code rows above @c k.
    double      r_max{0.0};      ///< Largest standardized residual on the LOS.
    double      v_abs_max{0.0};  ///< Largest |v| (metres) on the LOS.
    std::string signals;         ///< ';'-joined distinct non-empty signal labels of the rows over @c k (event detail).
    /// The bar this verdict was taken at: @c min_rows under the fixed rule,
    /// @c min(min_rows, n_rows) under the available one (RF-3b B1).  Written to the
    /// event detail so a declaration states the bar it cleared rather than the
    /// configured constant.
    int         min_rows_eff{0};
    /// The evidence clears the available bar and the LOS has fewer code rows than the
    /// configured @c min_rows: the population the fixed rule cannot reach (RF-3b B5).
    /// Computed under either rule, so an arm with the fixed rule still reports it.
    bool        short_over{false};
};

/**
 * @brief Pure function: adjudicate one line of sight's code rows for this epoch.
 *
 * The rule is a free function so it can be unit-tested without a network,
 * following the @c networkRaimEraseBandScope precedent.
 *
 * Two edge cases:
 *  - **Counted as rows, not distinct signal labels.**  Before card NOM step N2b a
 *    row's signal was read from its @c CB: column, so a code row of a posture whose
 *    satellite code biases are supplied externally, and a row of the W6 datum signal,
 *    had an empty label; a distinct-label test would have under-counted exactly
 *    there.  The signal now comes from the row's tag, and the rule still counts rows.
 *    The distinct signals that were over the bar reach the event.
 *  - **@c under_clear is not `!over`.**  The release bar is far below the
 *    declaration bar, and the band between them is evidence neither to declare nor
 *    to release: a line of sight sitting there keeps its current disposition,
 *    which is what gives the hysteresis effect.  An empty row set gives
 *    @c under_clear = false for the same reason: absence is not evidence of health
 *    (standing invariant #1).
 *
 * @param rows        This LOS's code rows this epoch (may be empty).
 * @param k           Declaration bar, in units of the row's own σ.
 * @param min_rows    Rows that must clear @p k together (≥ 1).
 * @param clear_sigma Release bar, same units.
 * @param min_rows_available Read the bar as @c min(min_rows, the LOS's code rows this
 *                    epoch) instead of the constant, so a line of sight with fewer rows
 *                    than the constant is reachable while one with more keeps the
 *                    multi-band corroboration (RF-3b B1,
 *                    @c network.gross_los_min_rows_rule).
 *                    @ref GrossLosVerdict::short_over is filled under either rule.
 * @return The verdict.
 */
GrossLosVerdict networkGrossLosAssess(const std::vector<GrossLosSample>& rows,
                                      double k, int min_rows, double clear_sigma,
                                      bool min_rows_available = false);

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
    /// TOW of the epoch that opened the current over-bar streak (0 = none; RF-3b B5).
    /// The origin of the per-episode latency, so a declaration states how long the
    /// evidence stood before the response acted, not only when it acted.
    double first_over_tow{0.0};
    /// Over-bar screened epochs since @ref first_over_tow: the same latency in epochs (RF-3b B5).
    int    first_over_age_ep{0};
    /// The standing declaration was taken under the cold-start bound (card ICF response
    /// 1).  While set, each epoch over the bound re-erases the carried ambiguity, and the
    /// release or expiry re-seeds it.  Cleared at the release and the expiry.
    bool   cold{false};
};

/// The edge a @ref networkGrossLosStep transition produced.
enum class GrossLosEdge {
    None,       ///< No edge this epoch.
    Declare,    ///< Decoupling edge: the LOS became gross.
    Clear,      ///< Re-coupling edge: the LOS was released.
    Suppressed  ///< The evidence was complete and a guard held the declaration (counted, never silent).
};

/**
 * @brief Pure function: advance one line of sight's state machine by one screened epoch.
 *
 * Called only for an epoch in which the LOS produced code rows.  The caller
 * handles absence (the streak reset and, past @c gross_los_max_gap_s, the expiry
 * edge), because "unobserved" is not a transition of this machine.
 *
 * @p allow_declare never blocks a clear.  Releasing a line of sight the data show
 * to be healthy is always safe, and a guard that also froze the release edge would
 * leave a response with no exit (standing invariant #7's re-coupling edge failing
 * closed).
 *
 * When @p allow_declare is false, the completed streak is held at
 * @p declare_epochs, neither discarded nor allowed to grow.  The evidence is real,
 * so discarding it would lose it, and letting it accumulate would turn the guard's
 * opening into a burst of simultaneous declarations across every line of sight
 * that waited.  With the streak held, the line of sight declares on the first
 * qualifying epoch after the guard opens and on no earlier one.
 *
 * @param s             [in/out] the LOS's carried state.
 * @param v             This epoch's verdict.
 * @param declare_epochs Consecutive over-bar epochs required (≥ 1).
 * @param clear_epochs   Consecutive under-bar epochs required (≥ 1).
 * @param tow           This epoch's TOW (stamps a declaration).
 * @param allow_declare False when a guard forbids a new declaration this epoch: the
 *                      maturity horizon (@c network.gross_los_warmup_epochs), the
 *                      whole-station stand-down (@c network.gross_los_max_station_frac),
 *                      a stood-down satellite (GL-2) or a standing station-level
 *                      declaration (RF-3b B4).
 * @return the edge, if any; @c GrossLosEdge::Suppressed when a guard held a
 *         complete declaration (a population with its own run-end counter, so a
 *         guard is never silent: standing invariant #4).
 */
GrossLosEdge networkGrossLosStep(GrossLosState& s, const GrossLosVerdict& v,
                                 int declare_epochs, int clear_epochs, double tow,
                                 bool allow_declare = true);

/// The edge a @ref networkGrossLosColdStep transition produced (card ICF response 1).
enum class GrossLosColdEdge {
    None,     ///< The cold-start bound does not act on this line of sight this epoch.
    Declare,  ///< Decoupling edge: the line of sight is declared under the bound.
    Refloat,  ///< A standing cold declaration whose bound was exceeded again: erase its carried ambiguity again.
    Held      ///< Over the bound inside the horizon, and the cold station guard held the declaration (counted, never silent).
};

/**
 * @brief Pure function: the cold-start bound's transition for one line of sight
 *        (card ICF response 1, @c network.gross_los_cold_bound_m).
 *
 * Called for an epoch in which the line of sight produced code rows, before
 * @ref networkGrossLosStep.  A @c Declare edge replaces that step for the epoch; the
 * other edges leave it to run (it owns the release test of every declaration).
 *
 *  - A bound of 0 or less is off: @c None, whatever the residual.
 *  - A line of sight not declared, inside the horizon, whose largest code |v| exceeds
 *    the bound is declared at once (@c Declare: @c gross and @c cold set, both streaks
 *    reset, the episode counted), unless @p allow_declare is false (@c Held).
 *  - A line of sight declared under the bound whose largest code |v| exceeds it again
 *    gives @c Refloat, inside the horizon or after it: the re-erase belongs to the
 *    declaration, which outlives the horizon until its release.
 *  - A line of sight declared on the σ bar is never touched here.
 *
 * Equality with the bound is not over it.  An empty row set is never over (absence is
 * not evidence, the screen's rule).
 *
 * @param s             [in/out] the line of sight's carried state.
 * @param v             This epoch's verdict (@ref GrossLosVerdict::v_abs_max is the statistic).
 * @param bound_m       The bound, metres (0 = off).
 * @param in_hold       True while the maturity horizon has not opened.
 * @param allow_declare False when the cold station guard holds new cold declarations at
 *                      this station this epoch.
 * @param tow           This epoch's TOW (stamps a declaration).
 * @return The edge.
 */
GrossLosColdEdge networkGrossLosColdStep(GrossLosState& s, const GrossLosVerdict& v,
                                         double bound_m, bool in_hold, bool allow_declare,
                                         double tow);

/**
 * @brief Run-end readout of the cold-start bound (card ICF response 1; standing
 *        invariant #4).
 *
 * Prints nothing when @c network.gross_los_cold_bound_m is 0.  Armed, it states the
 * population screened inside the horizon, the largest residual over the bound and the
 * largest under it on an undeclared line of sight (the bound's margin on this run), both
 * edges and the re-erases with their carry counts, the station guard, and the
 * declarations by line of sight.  An armed bound that declared nothing prints an INERT
 * clause naming which of three cases it was (never screened inside the horizon, nothing
 * reached the bound, or the guard held everything), and a bound set but resolved off
 * (the screen off, or the hold on) prints a warning.
 *
 * Called once by its row of the run-end table (the overload taking the view, below),
 * right after the screen's own report, networkGrossLosRunEnd.
 *
 * @param ctx     Run context (knobs, the per-LOS states, the tallies).
 * @param config_ Live config (the configured bound and the horizon).
 * @param log     "main_log".
 */
void networkGrossLosColdBoundRunEnd(const NetworkRunContext& ctx,
                                    const GnssConfig& config_,
                                    const std::shared_ptr<spdlog::logger>& log);

/**
 * @brief RF-3b B3: which half of the screen a call runs.
 *
 * The screen has two halves with opposite ordering constraints.  The decision
 * (assess the post-fit code rows, advance the state machine, emit both edges,
 * publish the deweight) needs only the solved epoch, and must run before the
 * emission for a declaration to reach the epoch it declares.  The response (erase
 * or hold the carried level, erase the per-LOS iono, mark pending-reseed) must run
 * after the carry harvest, or the harvest re-stashes what it removed.  With
 * @c network.gross_los_emit_exclude off, both run in one call at the post-harvest
 * site and the output is byte-identical to the unsplit screen; with it on, they
 * are split across the epoch.
 */
enum class GrossLosPhase {
    Full,    ///< (default) decide and respond in one call at the post-harvest site.
    Decide,  ///< Decide only; the declarations and releases are deferred to @ref networkGrossLosAct.
    Act      ///< Apply the deferred response of this epoch's @c Decide pass.
};

/**
 * @brief RF-3b B3: apply the response the decision half deferred.
 *
 * Runs at the post-harvest site: at the end of a @c Full pass, or from the
 * driver's @c Act call when the seam is split.  For every line of sight declared
 * this epoch (directly, or through a station-level declaration) it applies the
 * carry response: the erase, or the hold under @c network.gross_los_bound.  For
 * every one released or expired it drops the hold.  Under the hold, while any
 * carried level is held, it re-applies the held entries (levels and iono) over
 * whatever the harvest re-stashed, which keeps the folded code row's residual a
 * measure of the fault rather than a random walk.
 *
 * Returns immediately when the screen is off.
 *
 * @param ctx      [in/out] the carry (the two maps the response reaches, the holds) and counters.
 * @param log      "main_log".
 * @param stations [in/out] @c sm->markAmbiguityPendingReseed on a declaration.
 * @param asm_res  Per-station assembly: the ambiguity pool the response walks.
 */
void networkGrossLosAct(NetworkRunContext& ctx,
                        const std::shared_ptr<spdlog::logger>& log,
                        std::vector<NetStation>& stations,
                        const std::vector<StationAssembly>& asm_res);

/**
 * @brief Run the gross-LOS screen for this epoch (the seam).
 *
 * Walks each station in the solve once, forms @f$v = y - A_s x_s - A_r x_r@f$,
 * takes the code observation rows by their contribution tags (the rule
 * @c networkPostfitSigma0 reads; card NOM step N2b), bins them per satellite, and
 * drives the state machine above.
 *
 * Mutates persistent carries.  The response erases (or, under
 * @c network.gross_los_bound, holds) @c ctx.carry.local_carry entries and marks
 * StateManager ambiguities pending-reseed for every declared LOS, and erases (or
 * holds) @c ctx.carry.iono_carry / @c iono_blk_carry for it.  The decision
 * rewrites @c ctx.carry.gl_state (and @c gl_sta_state, @c gl2_sat_down), which
 * the next epoch's assembly reads through @c NetStation::gl_code_sigma_scale.
 * The response must therefore run after the carry harvest and before the epoch
 * stash tail; the caller places the @c Full or @c Act call immediately after
 * @ref networkPostFitRaim inside the @c SB_RAIM bracket.  A @c Decide call runs
 * earlier, before the emission (see @ref GrossLosPhase).
 *
 * No-op (not even a state allocation) unless @c ctx.knobs.gl_on.
 *
 * @param ctx           [in/out] knobs, carry (streaks + declared set + the two
 *                      carry maps it erases from) and counters.
 * @param config_       Live config: the @c network.gross_los_* block.
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
 * @param phase         RF-3b B3: which half to run.  @c Full runs both in one call.
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
                           double now_s,
                           GrossLosPhase phase = GrossLosPhase::Full);

/**
 * @brief Republish each station's code-row σ scale from the declared set.
 *
 * The deweight actuator is the code counterpart of the signal-integrity card's
 * @c NetStation::phase_sigma_scale, and it follows the same thread-safety
 * contract: this function runs serially, before the parallel PASS-B produce, and
 * the assembly only find-reads the map.  An empty map is the off path: the
 * assembly's guard is `if (!st.gl_code_sigma_scale.empty())`, so an unarmed run
 * keeps the exact @c cwiseInverse weights with no extra floating-point
 * operations.
 *
 * Idempotent and cheap: it rebuilds every map from @c ctx.carry.gl_state (the
 * declared lines of sight) and @c ctx.carry.gl_sta_state (the declared stations,
 * one entry per satellite last screened there), so the work is O(number of
 * currently declared lines of sight).
 *
 * @param ctx      Run context (reads @c carry.gl_state, @c carry.gl_sta_state and @c knobs).
 * @param stations [in/out] each station's @c gl_code_sigma_scale.
 */
void networkGrossLosPublishScales(NetworkRunContext& ctx,
                                  std::vector<NetStation>& stations);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the gross line-of-sight screen.
 *
 * The gross line-of-sight screen (card GL): denominators, declarations and releases, the
 * guards, the response, the RF-3b per-rule lines, the GL-2 satellite stand-downs and the
 * identity tables. The folded-row count is summed from the stations into a local copy of
 * the tallies (it was summed into the run context).
 *
 * Gate: each section under its own condition, as in the teardown: always; `g.epochs > 0
 * && g.rows_screened == 0`; `g.los_screened > 0 && g.los_over == 0`;
 * `ctx.knobs.amb_float_kf`; always (a block); `g.declared > 0 && g.rows_deweighted ==
 * 0`; `g.los_screened > 0`; `ctx.knobs.gl2_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkGrossLosRunEnd(const NetworkRunEndView& v);

/**
 * @brief Run-end report: the gross line-of-sight screen's cold-start bound.
 *
 * The table's row for the cold-start bound of the gross line-of-sight screen (card ICF
 * response 1); the report is the overload above, which a unit test calls directly.
 *
 * Gate: none of its own: the row calls the report above on every run, as the teardown
 * did, and that report applies its own.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkGrossLosColdBoundRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
