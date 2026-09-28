#pragma once
/**
 * @file NetworkSdEwlAr.hpp
 * @brief Network producer epoch block: the SD extra-wide-lane integer decision, Route B
 *        (between-station DD of the SD EWL) with the A2 DD-to-SD datum propagation
 *        (docs/SPEC_sd_ewl_prereduce.md §8.2 (increment 2a), as amended by
 *        docs/SPEC_sd_ewl_prereduce.md §10).
 *
 * ## What it decides, and why Route B
 *
 * Increment 1 folds a float pseudo-observation row per (station, satellite != pivot)
 * on the constellation's EWL band pair:
 *
 *     z_SD = MW(r,s) - MW(r,p) = lambda_EWL * grad_N_EWL + [B_EWL^s - B_EWL^p]
 *
 * and gains only +0.51 % of ev1 at a realistic row sigma, because `MW = phi_WL - P_NL`
 * is a linear combination of observables the epoch's normal equations already hold, so
 * a float MW row adds no rank.  The new information is integrality, which increment 1
 * discards and this seam recovers.
 *
 * Rounding `z_SD / lambda_EWL` directly requires `B^s - B^p` to be removed first, and
 * that is what the network is solving for.  Route B avoids the circularity with the
 * between-station double difference:
 *
 *     [MW(A,s) - MW(A,p)] - [MW(B,s) - MW(B,p)]
 *         = lambda_EWL * [(N_As - N_Ap) - (N_Bs - N_Bp)]
 *
 * `B^s - B^p` cancels exactly (same satellite pair on both sides; each station's
 * receiver term already cancelled inside its own SD), leaving a pure integer
 * combination against decimetre-level averaged MW noise at lambda_EWL = 5.861 m
 * (GPS 2/5) or 9.768 m (Galileo 7/5).  An integer that uses no a-priori cannot be
 * corrupted by one, which is why Route B is preferred over rounding against a BIA
 * satellite bias.  Route A is also unavailable on the 50-station fixture: all 4138 EWL
 * arcs there report `bia_corrected = false`.
 *
 * ## Exactness precondition not stated in the SPEC: channel identity
 *
 * `B_EWL^s` is a per-signal combination of the satellite phase and code biases, so the
 * cancellation above holds only if both stations' MW arcs sit on the same two tracking
 * channels.  They need not: the detector chooses the channel of each arc, and the choice
 * varies across the network (41 % of resolved arcs had two candidate pairs; for
 * example `L7X/L5X` and `L7Q/L5Q` both win at different satellites, as do `L2W/L5X` and
 * `L2L/L5Q`).  Differencing across attributes folds a per-signal bias difference in as
 * if it were an integer.
 *
 * This seam therefore elects, per (constellation, satellite), a network-common channel
 * pair by station consensus (most stations holding a mature arc on it; ties broken by
 * ascending pair name for determinism) and admits only stations holding that pair.
 * Excluded stations are counted (@c sdewl_dd_skip_channel).
 *
 * ## A2: the DD-to-SD integer datum
 *
 * Route B yields DD integers; the row needs an SD integer per (station, satellite).  Per
 * constellation (the station graph is measured to form one connected component), a
 * reference station r0 is elected deterministically, `N_SD(r0) == 0` is defined, and
 * spanning-tree DD fixes are propagated outward:
 *
 *     n(r0, s) = 0                                      for every satellite at r0
 *     n(r,  s) = n(q, s) - round(DD(q,r; s,s*))         q = tree parent, s* = link sat
 *     n(r, s*) = n(q, s*)                               the free per-station integer a_r
 *
 * Stations not reachable from r0 get no integer and are counted as no-link.  The value
 * the row then asserts is
 *
 *     grad_N(r; s,p) = n(r,s) - n(r,p)
 *                    = grad_N_true(r; s,p) - grad_N_true(r0; s,p)
 *
 * i.e. the true SD integer minus a per-(satellite pair, component) constant.  The bias
 * combination the network then observes is the true `B^s - B^p` plus
 * `lambda * N_SD_true(r0)`, which is the classical WL-FCB modulo-lambda definition.  This
 * is harmless within the run and to any consumer integer decision.  Production arming
 * must route fix onset through the per-lambda disc contract (PB gate v2); otherwise the
 * consumer wire sees a phase-bias step.
 *
 * Two structural consequences:
 *  - `a_r`, and with it the spanning tree and any tree churn, cancels in the SD at use.
 *    Only the identity of r0 sets the frame, so the seam keeps r0 sticky and counts
 *    re-elections (@c sdewl_sd_ref_reelected) instead of stabilizing the tree.
 *  - r0's own true SD integers are absorbed into that constant, so this is a datum
 *    choice, not a measurement: the link satellite s* is not gated.
 *
 * ## Standing invariant #1: why a per-epoch re-decision still needs an event response
 *
 * Increment 2c (the zero-q hold) is not built here.  The decision is re-derived from
 * scratch every epoch, and nothing is carried forward except for flip detection.  That
 * alone does not satisfy invariant #1: the fixed row writes a lambda-quantized value
 * (5.861 / 9.768 m) into the satellite PB/CB columns, which run at
 * `sat_phase_bias_q = 1e-12` / `sat_code_bias_q = 1e-10`, a carry whose measured
 * response to a step has tau ~ 3.8 h with the discontinuity counter silent throughout.
 * An integer that changes between epochs therefore injects the failure mode the
 * invariant describes.
 *
 * The wired response, in this same increment: when this epoch's integer disagrees with
 * the previous epoch's for the same (station, constellation, satellite) on a continuous
 * arc, the disagreement is counted (@c sdewl_dd_flip) and the fix is withheld for that
 * arc this epoch (reset over repair: on a flip epoch neither value is known to be
 * right).  A restarted arc (the detector's lagged sample count went down: slip, gap or
 * re-acquisition) replaces the memory without charging a flip, since the true integer
 * may legitimately have changed.  The memory is used only to veto; the decision itself is
 * never reused, which keeps this separate from 2c's zero-q hold.
 *
 * ## Ordering contract
 *
 * Runs serially inside the caller's SB_PREASM bracket, after
 * `networkAssemblyPrefilter_` (satellites seen by < 2 stations are erased there, and an
 * integer decided for a satellite that then vanishes is wasted work), after the
 * phase-datum election and the eq34 RPB allocation (both mutate the `StateManager`
 * layout), after `networkAssemblyRegisterColumns_` (which can still skip the epoch), and
 * before `networkPassB_`, whose parallel produce loop reads the published result.  The
 * published map is therefore written once per epoch and read-only during produce; a lazy
 * fill or a memo written inside produce would race.
 *
 * Owns no state: `ctx.carry.sd_ewl_*` and `ctx.counters.sdewl_*` are the storage, per
 * the anti-regrowth rule that carried state never lives in a function-local.
 *
 * Off by default (`network.sd_ewl.fix_enable`): the function clears the published map
 * and returns, so the output is byte-identical.
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
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp, included in the .cpp only

/// One station's view of one satellite's EWL arc, reduced to what Route B needs.
struct SdEwlArcStat {
    double      u_cyc{ 0.0 };     ///< the ZD-consistent MW mean divided by lambda_EWL (cycles).
    double      var_cyc2{ 0.0 };  ///< variance of that mean (cycles^2), already floored.
    std::size_t count{ 0 };       ///< the detector's lagged Welford sample count (maturity + link ranking).
};

/// One propagated integer plus the quality of the DD that produced it.
struct SdEwlDecision {
    long long n_cyc{ 0 };       ///< integer in the r0-anchored frame (see the file banner).
    double    frac_cyc{ 0.0 };  ///< |fractional residue| of its DD; 0 for a datum (r0 / link) value.
    double    ffr{ 1.0 };       ///< rounding success probability of its DD; 1 for a datum value.
    bool      measured{ false };///< true iff produced by a rounded DD (false = a frame/datum choice, which no gate can reject).
};

/// Tallies of one @ref sdEwlPropagate call (the caller adds them into ctx.counters).
struct SdEwlPropagateTally {
    long long attempts{ 0 };      ///< DD roundings attempted.
    long long fixed{ 0 };         ///< integers produced by a rounding (excludes datum values).
    long long reject_frac{ 0 };   ///< rejected on |fractional residue|.
    long long reject_ffr{ 0 };    ///< rejected on rounding success probability.
    long long datum{ 0 };         ///< values assigned as a datum (r0's satellites, each station's link satellite, and frame extensions) rather than measured.
};

/// How this epoch's integer relates to the previous epoch's for the same arc: the
/// standing-invariant-#1 event classification.
enum class SdEwlFlipVerdict {
    Fresh,    ///< no previous decision (first epoch, after a warm resume, or after the arc's memory was dropped): fold.
    Stable,   ///< same integer on a continuous arc: fold.
    Restart,  ///< the arc restarted (the detector's lagged count fell: slip, gap or re-acquisition), so the true integer may legitimately have changed.  Replace the memory, charge no flip, fold.
    Flip      ///< a different integer on a continuous arc.  Count it and fold neither value: a lambda-quantized step into a tau ~ 3.8 h PB/CB carry is the failure the invariant names, and on a flip epoch neither value is known to be right.
};

/**
 * @brief Classify this epoch's integer against the previous epoch's for one arc.
 *        Pure, so the invariant-#1 response is unit-testable rather than reachable only
 *        through a multi-epoch field run.
 *
 * The restart test is on the sample count, not on a slip flag: the MW accumulator resets
 * on slip, gap and re-acquisition alike, so a falling lagged count covers every case
 * without depending on the slip funnel (which is itself `carry_invariant`-gated and
 * therefore empty in some legal configurations).
 */
SdEwlFlipVerdict sdEwlFlipCheck(bool have_prev, long long n_prev, std::size_t count_prev,
                                long long n_now, std::size_t count_now);

/**
 * @brief Bootstrapped success probability of rounding a scalar of noise @p sigma_cyc
 *        (cycles) to the nearest integer: @f$P(|e|<1/2)=\mathrm{erf}(0.5/(\sigma\sqrt2))@f$.
 *        FFR rather than ADOP, the AR validation criterion used in this codebase.
 *        Returns 1 for a non-positive sigma.
 */
double sdEwlRoundingFfr(double sigma_cyc);

/**
 * @brief Route-B DD rounding plus the A2 DD-to-SD datum propagation, as pure algebra over
 *        a name-keyed graph.  Kept separate from the seam so that the property that
 *        matters is unit-testable without a detector, a station roster or a solver.
 *
 * @param arcs  station name -> satellite -> arc statistic, all on one constellation and
 *              one network-common channel pair (the caller enforces both; the file
 *              banner explains why channel identity is a correctness precondition).
 * @param r0    the component reference station.  Its satellites are assigned 0 by
 *              definition; every other value is propagated relative to it.
 * @param max_frac_cyc  reject a DD whose |fractional residue| exceeds this.
 * @param min_ffr       reject a DD whose rounding success probability is below this.
 * @param[out] tally    per-call tallies.
 * @return station name -> satellite -> decision.  A satellite absent from a station's
 *         map got no integer (no admissible link reached it); the caller counts that.
 *
 * @par Guaranteed invariant
 * For any station r and any two satellites s, p that both received an integer,
 * @f$n(r,s)-n(r,p)@f$ equals the true SD integer @f$\nabla N(r;s,p)@f$ minus
 * @f$\nabla N(r_0;s,p)@f$, the same constant for every station in the component.  It is
 * therefore independent of the spanning tree and of each station's link satellite
 * (those only shift @f$n@f$ by a per-station constant, which the between-satellite
 * difference removes), so only the identity of @p r0 sets the frame.  `SdEwlDatumTest`
 * pins this property.
 */
std::map<std::string, std::map<std::string, SdEwlDecision>> sdEwlPropagate(
    const std::map<std::string, std::map<std::string, SdEwlArcStat>>& arcs,
    const std::string& r0,
    double max_frac_cyc,
    double min_ffr,
    SdEwlPropagateTally& tally);

/**
 * @brief The extra-wide-lane band pair of a constellation, with its two nominal
 *        frequencies: the single definition shared by the integer decision (2a) and
 *        the row builder (`Application::networkAppendSdEwlRows_`, 2b).
 *
 * GPS uses (L2, L5) with lambda_EWL = c/(f2-f5) = 5.861 m; Galileo uses (E5b, E5a)
 * with 9.768 m.  BeiDou is deferred (clock divergence) and GLONASS is excluded (FDMA:
 * a per-satellite frequency makes the between-satellite lane non-integer); both produce
 * no rows, so neither needs a skip counter.
 *
 * Nominal frequencies are exact here because GLONASS never reaches this table, so no
 * per-satellite frequency lookup is needed and the function has no non-const dependency
 * that would be unsafe in the parallel produce phase.  Keeping it in one place ensures
 * the decision and the row it feeds agree on the lane.
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
 * @param ctx     Run context: owns the published fix map, the sticky r0, the
 *                previous-epoch integers used for flip detection, and the counters.
 * @param config_ Run configuration (reads `network.sd_ewl`).
 * @param log     Run logger (per-epoch summary at debug level).
 * @param traces  Trace sinks (`network_sd_ewl_ar.csv`, armed only with
 *                `sd_ewl.trace && sd_ewl.fix_enable`).
 * @param setup   Station roster (MW detectors + state managers).
 * @param se      This epoch (week/tow for the trace).
 * @param now_s   Continuous GPS seconds.  Currently unused: arc continuity is judged by
 *                the detector's lagged sample count.  A time-based continuity test would
 *                use this clock, never an epoch index (`epoch_decimation_n`, the
 *                `start_offset_hours` fast-forward and skipped epochs all break index
 *                arithmetic).
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
