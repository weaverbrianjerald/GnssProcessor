#pragma once
/**
 * @file NetworkSdEwlAr.hpp
 * @brief Network producer epoch block: the SD extra-wide-lane INTEGER DECISION —
 *        Route B (between-station DD of the SD EWL) with the A2 DD-to-SD datum
 *        propagation (docs/SPEC_sd_ewl_integer_hold.md increment 2a, as AMENDED by
 *        docs/SPEC_lane_spectrometer.md §6).
 *
 * ## What it decides, and why Route B
 *
 * Increment 1 folds a FLOAT pseudo-observation row per (station, satellite != pivot)
 * on the constellation's EWL band pair:
 *
 *     z_SD = MW(r,s) - MW(r,p) = lambda_EWL * grad_N_EWL + [B_EWL^s - B_EWL^p]
 *
 * and buys +0.51 % of ev1 at an honest row sigma — because `MW = phi_WL - P_NL` is a
 * linear combination of observables the epoch's normal equations already hold, so a
 * FLOAT MW row adds ZERO new rank.  The only genuinely new content is INTEGRALITY,
 * which increment 1 discards.  This seam recovers it.
 *
 * Rounding `z_SD / lambda_EWL` directly needs `B^s - B^p` removed first, which is
 * what the network is solving for — circular.  Route B breaks the circle with the
 * BETWEEN-STATION double difference:
 *
 *     [MW(A,s) - MW(A,p)] - [MW(B,s) - MW(B,p)]
 *         = lambda_EWL * [(N_As - N_Ap) - (N_Bs - N_Bp)]
 *
 * `B^s - B^p` cancels EXACTLY (same satellite pair on both sides; each station's
 * receiver term already cancelled inside its own SD), leaving a pure integer
 * combination against decimetre averaged MW noise at lambda_EWL = 5.861 m (GPS 2/5)
 * or 9.768 m (Galileo 7/5).  An integer that uses no a-priori CANNOT be poisoned by
 * one — which is why Route B is preferred over rounding against a BIA satellite bias
 * (measured: all 4138 EWL arcs on the 50-station fixture came back
 * `bia_corrected = false`, so Route A is blocked anyway).
 *
 * ## The exactness precondition the SPEC did not state: CHANNEL IDENTITY
 *
 * `B_EWL^s` is a per-SIGNAL combination of the satellite phase and code biases, so
 * the cancellation above holds only if BOTH stations' MW arcs sit on the SAME two
 * tracking channels.  They need not: which channel the detector holds an arc on is
 * ITS choice, and it is measurably diverse across the network — 41 % of resolved arcs
 * had two candidate pairs, and both `L7X/L5X` (1242 arcs) and `L7Q/L5Q` (1034) won at
 * DIFFERENT satellites; likewise `L2W/L5X` (959) and `L2L/L5Q` (844).  Differencing
 * across attributes folds a per-signal bias difference in as if it were an integer.
 *
 * This seam therefore elects, per (constellation, satellite), a NETWORK-COMMON
 * channel pair by station consensus (most stations holding a mature arc on it;
 * tie-break ascending pair name for determinism) and admits only stations holding
 * that exact pair.  Excluded stations are counted (@c sdewl_dd_skip_channel), never
 * silently dropped.
 *
 * ## A2 — the DD-to-SD integer datum
 *
 * Route B yields DD integers; the row needs an SD integer per (station, satellite).
 * Per connected component (measured: one per constellation), elect a reference
 * station r0 deterministically, define `N_SD(r0) == 0`, and propagate spanning-tree
 * DD fixes outward:
 *
 *     n(r0, s) = 0                                      for every satellite at r0
 *     n(r,  s) = n(q, s) - round(DD(q,r; s,s*))         q = tree parent, s* = link sat
 *     n(r, s*) = n(q, s*)                               the free per-station integer a_r
 *
 * The value the row then asserts is
 *
 *     grad_N(r; s,p) = n(r,s) - n(r,p)
 *                    = grad_N_true(r; s,p) - grad_N_true(r0; s,p)
 *
 * i.e. the true SD integer MINUS a per-(satellite-pair, component) CONSTANT.  The
 * bias combination the network then observes is the true `B^s - B^p` PLUS
 * `lambda * N_SD_true(r0)` — exactly the classical WL-FCB modulo-lambda definition.
 * Harmless in-run and to any consumer integer decision; production arming must route
 * fix-onset through the per-lambda disc contract (PB gate v2) or the consumer wire
 * sees a phase-bias step.
 *
 * Two structural consequences worth knowing, both load-bearing:
 *  - `a_r` (and hence the spanning TREE, and hence tree churn) CANCELS in the SD at
 *    use.  Only the identity of r0 sets the frame.  So the seam keeps r0 STICKY and
 *    counts re-elections (@c sdewl_sd_ref_reelected) rather than defending the tree.
 *  - r0's own true SD integers are absorbed into that constant, so this is a DATUM
 *    choice, not a measurement: the link satellite s* is deliberately NOT gated.
 *
 * ## Standing invariant #1 — why a per-epoch re-decision still needs an event response
 *
 * 2c (the zero-q hold) is NOT built here; the decision is re-derived from scratch
 * every epoch and nothing is carried forward except for FLIP DETECTION.  That does
 * not by itself escape invariant #1: the fixed row writes a lambda-quantized value
 * (5.861 / 9.768 m) into the satellite PB/CB columns, which already run at
 * `sat_phase_bias_q = 1e-12` / `sat_code_bias_q = 1e-10` — a carry whose measured
 * response to a step is tau ~ 3.8 h with the discontinuity counter silent throughout.
 * So an integer that CHANGES between epochs injects exactly the failure mode the
 * invariant exists to prevent.
 *
 * The wired response, in this same increment: when this epoch's integer disagrees
 * with the previous epoch's for the same (station, constellation, satellite) on a
 * CONTINUOUS arc, the disagreement is counted (@c sdewl_dd_flip) and the fix is
 * WITHHELD for that arc this epoch — reset over repair, because on a flip epoch we do
 * not know which value was right.  A restarted arc (the detector's lagged sample
 * count went DOWN, i.e. slip / gap / re-acquisition) replaces the memory WITHOUT
 * charging a flip: the true integer legitimately changed.  Note the memory is used
 * only to VETO; the decision itself is never reused, which is what keeps this out of
 * 2c's zero-q hold territory.
 *
 * ## Ordering contract
 *
 * Runs SERIALLY inside the caller's SB_PREASM bracket, AFTER
 * `networkAssemblyPrefilter_` (satellites seen by < 2 stations are erased there, and
 * an integer decided for a satellite that then vanishes is wasted work), AFTER the
 * phase-datum election and the eq34 RPB allocation (both mutate `StateManager` layout),
 * and BEFORE `networkPassB_`, whose PARALLEL produce loop READS the published result.
 * The published map is therefore write-once-per-epoch and read-only from produce — a
 * lazy fill or a memo written inside produce would race.
 *
 * Owns NO state: `ctx.carry.sd_ewl_*` and `ctx.counters.sdewl_*` are the storage, per
 * the anti-regrowth rule that carried state never lives in a function-local.
 *
 * Default OFF (`network.sd_ewl.fix_enable`) => returns immediately after clearing the
 * published map => byte-identical.
 */

#include "app/NetworkSetupResult.hpp"
#include "common/PppObservation.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/// One station's view of one satellite's EWL arc, reduced to what Route B needs.
struct SdEwlArcStat {
    double      u_cyc{ 0.0 };     ///< the ZD-consistent MW mean divided by lambda_EWL (cycles).
    double      var_cyc2{ 0.0 };  ///< variance OF THAT MEAN (cycles^2), already floored.
    std::size_t count{ 0 };       ///< the detector's lagged Welford sample count (maturity + link ranking).
};

/// One propagated integer plus the quality of the DD that produced it.
struct SdEwlDecision {
    long long n_cyc{ 0 };       ///< integer in the r0-anchored frame (see the file banner).
    double    frac_cyc{ 0.0 };  ///< |fractional residue| of its DD; 0 for a datum (r0 / link) value.
    double    ffr{ 1.0 };       ///< rounding success probability of its DD; 1 for a datum value.
    bool      measured{ false };///< true iff produced by a ROUNDED DD (false = a frame/datum choice, which no gate can reject).
};

/// Tallies of one @ref sdEwlPropagate call (the caller adds them into ctx.counters).
struct SdEwlPropagateTally {
    long long attempts{ 0 };      ///< DD roundings attempted.
    long long fixed{ 0 };         ///< integers produced by a rounding (excludes datum values).
    long long reject_frac{ 0 };   ///< rejected on |fractional residue|.
    long long reject_ffr{ 0 };    ///< rejected on rounding success probability.
    long long datum{ 0 };         ///< values assigned as a DATUM (r0's satellites + each station's link satellite) rather than measured.
};

/// How this epoch's integer relates to the previous epoch's for the SAME arc — the
/// standing-invariant-#1 event classification.
enum class SdEwlFlipVerdict {
    Fresh,    ///< no previous decision (first epoch, or after a warm resume): fold.
    Stable,   ///< same integer on a continuous arc: fold.
    Restart,  ///< the arc restarted (the detector's lagged count FELL: slip / gap / re-acquisition), so the true integer legitimately changed.  Replace the memory, charge NO flip, fold.
    Flip      ///< a DIFFERENT integer on a continuous arc.  Count it and fold NEITHER value: a lambda-quantized step into a tau ~ 3.8 h PB/CB carry is precisely the failure the invariant names, and on a flip epoch we do not know which value was right.
};

/**
 * @brief Classify this epoch's integer against the previous epoch's for one arc.
 *        Pure, so the invariant-#1 response is unit-testable rather than only reachable
 *        through a multi-epoch field run.
 *
 * The RESTART test is on the sample count, not on a slip flag: the MW accumulator resets
 * on slip, gap and re-acquisition alike, so a falling lagged count is the one signal that
 * covers every case without depending on the slip funnel (which is itself
 * `carry_invariant`-gated and therefore empty in some legal configurations).
 */
SdEwlFlipVerdict sdEwlFlipCheck(bool have_prev, long long n_prev, std::size_t count_prev,
                                long long n_now, std::size_t count_now);

/**
 * @brief Bootstrapped success probability of rounding a scalar of noise @p sigma_cyc
 *        (cycles) to the nearest integer: @f$P(|e|<1/2)=\mathrm{erf}(0.5/(\sigma\sqrt2))@f$.
 *        FFR, not ADOP — the standing AR validation criterion in this codebase.
 */
double sdEwlRoundingFfr(double sigma_cyc);

/**
 * @brief Route-B DD rounding + the A2 DD-to-SD datum propagation, as PURE algebra over
 *        a name-keyed graph.  Extracted from the seam so the property that actually
 *        matters is unit-testable without a detector, a station roster or a solver.
 *
 * @param arcs  station name -> satellite -> arc statistic, all on ONE constellation and
 *              ONE network-common channel pair (the caller enforces both — see the file
 *              banner on why channel identity is a correctness precondition).
 * @param r0    the component reference station.  Its satellites are assigned 0 BY
 *              DEFINITION; everything else is propagated relative to it.
 * @param max_frac_cyc  reject a DD whose |fractional residue| exceeds this.
 * @param min_ffr       reject a DD whose rounding success probability is below this.
 * @param[out] tally    per-call tallies.
 * @return station name -> satellite -> decision.  A satellite absent from a station's
 *         map got no integer (no admissible link reached it); the caller counts that.
 *
 * @par The invariant this guarantees
 * For any station r and any two satellites s, p that BOTH received an integer,
 * @f$n(r,s)-n(r,p)@f$ equals the true SD integer @f$\nabla N(r;s,p)@f$ minus
 * @f$\nabla N(r_0;s,p)@f$ — the same constant for every station in the component.  In
 * particular it is INDEPENDENT of the spanning tree and of each station's link
 * satellite (those only shift @f$n@f$ by a per-station constant, which the
 * between-satellite difference removes), so only the identity of @p r0 sets the frame.
 * `SdEwlDatumTest` pins exactly this.
 */
std::map<std::string, std::map<std::string, SdEwlDecision>> sdEwlPropagate(
    const std::map<std::string, std::map<std::string, SdEwlArcStat>>& arcs,
    const std::string& r0,
    double max_frac_cyc,
    double min_ffr,
    SdEwlPropagateTally& tally);

/**
 * @brief The extra-wide-lane band pair of a constellation, with its two nominal
 *        frequencies — the single definition shared by the integer decision (2a) and
 *        the row builder (`Application::networkAppendSdEwlRows_`, 2b).
 *
 * GPS uses (L2, L5) with lambda_EWL = c/(f2-f5) = 5.861 m; Galileo uses (E5b, E5a)
 * with 9.768 m.  BeiDou is deferred (clock divergence) and GLONASS excluded (FDMA:
 * a per-satellite frequency makes the between-satellite lane non-integer), and both
 * simply produce no rows — which is why neither needs a skip counter.
 *
 * Nominal frequencies are EXACT here precisely because GLONASS never reaches this
 * table, so no per-satellite frequency lookup is needed and the function stays free
 * of any non-const dependency that would be unsafe in the parallel produce phase.
 * Kept in ONE place so the decision and the row it feeds can never disagree about
 * which lane they are on.
 *
 * @param cons Constellation character ('G', 'E', ...).
 * @param b1   [out] higher-frequency band character.
 * @param b2   [out] lower-frequency band character.
 * @param f1   [out] frequency of @p b1 (Hz).
 * @param f2   [out] frequency of @p b2 (Hz).
 * @return true if the constellation has an EWL tier.
 */
bool sdEwlBands(char cons, char& b1, char& b2, double& f1, double& f2);

/**
 * @brief Decide this epoch's SD EWL integers by Route B and publish them for the
 *        PASS-B row builder.  Serial; mutates only `ctx.carry.sd_ewl_*` and
 *        `ctx.counters.sdewl_*`.  No-op (and clears the published map) unless
 *        `network.sd_ewl.enable && network.sd_ewl.fix_enable`.
 *
 * @param ctx     Run context — owns the published fix map, the sticky r0, the
 *                previous-epoch integers used for flip detection, and the counters.
 * @param config_ Run configuration (reads `network.sd_ewl`).
 * @param log     Run logger (per-epoch summary at debug, first-epoch info).
 * @param traces  Trace sinks (`network_sd_ewl_ar.csv`, armed only with
 *                `sd_ewl.trace && sd_ewl.fix_enable`).
 * @param setup   Station roster (MW detectors + state managers).
 * @param se      This epoch (week/tow for the trace).
 * @param now_s   Continuous GPS seconds — the arc-continuity clock, never an epoch
 *                index (`epoch_decimation_n`, the `start_offset_hours` fast-forward
 *                and skipped epochs all break index arithmetic).
 * @param modeled Per-station modelled observations (satellite visibility this epoch).
 * @param present Per-station presence flags for this epoch.
 */
void networkSdEwlAr(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    double now_s,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<bool>& present);

} // namespace GnssProcessor
