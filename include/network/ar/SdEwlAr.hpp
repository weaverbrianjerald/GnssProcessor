#pragma once
/**
 * @file SdEwlAr.hpp
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

#include "network/driver/SetupResult.hpp"
#include "common/PppObservation.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "network/driver/RunTypes.hpp"
#include "network/state/Entities.hpp"   // net::StationTable
#include "network/assembly/Contributions.hpp"
#include "math/TimeUtils.hpp"
#include <Eigen/Dense>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only
struct NetworkRunTraces;    // network/diag/RunTraces.hpp, included in the .cpp only

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

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Decide this epoch's SD EWL integers by Route B and publish them for the
 *        PASS-B row builder.  Serial; mutates only `ctx.carry.sd_ewl_*` and
 *        `ctx.counters.sdewl_*`.  No-op (and clears the published map) unless
 *        `network.sd_ewl.enable && network.sd_ewl.fix_enable`.
 *
 * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.setup (setup). Read: run.config (config_), run.log
 *            (log).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se),
 *           ep.clock.pl_now_s (now_s), ep.pass_a.modeled (modeled), ep.pass_a.present
 *           (present).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: Run context: owns the published fix map, the sticky r0, the
 *     previous-epoch integers used for flip detection, and the counters.
 *   - @p config_ = run.config: Run configuration (reads `network.sd_ewl`).
 *   - @p log = run.log: Run logger (per-epoch summary at debug level).
 *   - @p traces = run.traces: Trace sinks (`network_sd_ewl_ar.csv`, armed only with
 *     `sd_ewl.trace && sd_ewl.fix_enable`).
 *   - @p setup = run.setup: Station roster (MW detectors + state managers).
 *   - @p se = ep.se: This epoch (week/tow for the trace).
 *   - @p now_s = ep.clock.pl_now_s: Continuous GPS seconds. Currently unused: arc continuity
 *     is judged by the detector's lagged sample count. A time-based continuity test would use
 *     this clock, never an epoch index (`epoch_decimation_n`, the `start_offset_hours`
 *     fast-forward and skipped epochs all break index arithmetic).
 *   - @p modeled = ep.pass_a.modeled: Per-station modelled observations (satellite visibility
 *     this epoch).
 *   - @p present = ep.pass_a.present: Per-station presence flags for this epoch.
 */
void networkSdEwlAr(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the SD pre-reduce EWL rows and integer fix.
 *
 * The SD pre-reduce EWL constraint rows and their integer fix (2a/2b), summed over the
 * stations, with the dominant skip cause when nothing was built.
 *
 * Gate: returns 0 at once unless `config_.getNetworkConfig().sd_ewl.enable`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkSdEwlRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- PASS B station steps, declarations (generated by scripts/nom_pb1b_cut.py) ----

struct StationBuild;                       // network/assembly/StationBuild.hpp
struct StationCarryFrame;                  // network/assembly/StationBuild.hpp
enum class StationStep : std::uint8_t;     // network/assembly/StationBuild.hpp
class GnssConfig;
class NetworkEstimator;
class SphericalHarmonicIonosphere;
class GlobalIonosphereMap;
struct GlobalVtecShConfig;

/**
 * @brief The SD pre-reduce EWL constraint rows (docs/SPEC_sd_ewl_prereduce.md).
 *
 * Step `sd_ewl` of kStationBuildSteps (network/assembly/StationBuild.hpp); its kinds: SdEwl. It
 * returns at once unless `config_.getNetworkConfig().sd_ewl.enable`. Records: sd_ewl_trace,
 * sd_ewl_rows; st.sd_ewl_ctr. The frozen text of networkPassB_'s produce loop below its
 * bindings.
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkSdEwlStationRows(StationBuild& build);

/**
 * @brief Append the SD pre-reduce EWL constraint rows to one station's own block.
 *        See docs/SPEC_sd_ewl_prereduce.md.
 *
 * For each satellite @f$s@f$ paired against a pivot satellite @f$p@f$ on the
 * constellation's extra-wide-lane band pair @f$(b_1,b_2)@f$ (GPS 2/5, Galileo 7/5),
 * one pseudo-observation row
 * @f[
 *   z_{SD}=\overline{MW}(r,s)-\overline{MW}(r,p)
 *         =\lambda_{EWL}\big[N_{EWL}(r,s)-N_{EWL}(r,p)\big]+\big[B^s_{EWL}-B^p_{EWL}\big]
 * @f]
 * The pivot is elected here, per constellation: the arc with the most samples, ties
 * broken by ascending satellite name.  The receiver EWL bias is common to both arcs at
 * the same station and cancels identically, so the row carries only the SD ambiguity
 * combination and the SD satellite bias lump.  That lump is the reason for the row: it
 * reaches the shared satellite phase and code bias columns, which DD-chord rows barely
 * reach (about 80 % of them are eq34 tree pins; measured effect: +0.6 % on the
 * soft-end eigenvalue).
 *
 * Local coefficients (@p A_r), on the metre-valued per-band ambiguity states:
 * @f$+\lambda_{EWL}/\lambda_{b1}@f$, @f$-\lambda_{EWL}/\lambda_{b2}@f$ for @f$s@f$ and
 * negated for @f$p@f$.  Shared coefficients (@p A_s), from
 * @f$MW=\varphi_{WL}-P_{NL}@f$: @f$+f_1/(f_1-f_2)@f$ on @c PB|s|b1,
 * @f$-f_2/(f_1-f_2)@f$ on @c PB|s|b2, @f$-f_1/(f_1+f_2)@f$ on @c CB|s|b1,
 * @f$-f_2/(f_1+f_2)@f$ on @c CB|s|b2, negated for @f$p@f$.
 *
 * All rows of a group (one constellation at this station, split in two under
 * increment 2b below) share the pivot arc's noise, so
 * @f$R=\mathrm{diag}(\mathrm{var}_s)+\mathrm{var}_p\mathbf{1}\mathbf{1}^\top@f$,
 * structurally the GIM common-mode R.  R is whitened by @f$L^{-1}@f$
 * (@f$LL^\top=R@f$) into unit-information rows, as
 * @ref networkAppendGimIonoRows_ does.
 *
 * @par Why pre-reduce
 * These rows enter the station block that reaches @c addStationObservations, so they
 * fold into @f$N_{rr}/N_{sr}/N_{ss}@f$ before @c reduce() and therefore compose
 * exactly with the post-reduce DD fold.  Two post-reduce Woodbury folds over
 * coupled local columns drop the cross term (see the warning on
 * @c NetworkNormalEquations::foldReducedDdPriors in NormalEquations.hpp).
 * @c addStationPrior cannot be used: it is diagonal-only
 * (@f$N_{rr}(c,c)\mathrel{+}=\Lambda@f$) and cannot express this cross-column row.
 *
 * @par Which signal pair
 * The MW arc key carries the two phase signal names, and the tracking channel the
 * detector holds an arc on is the detector's choice (@c collectUncombinedPairs'
 * KF-chosen / attribute-priority pick).  This function therefore searches every
 * candidate (band1, band2) phase pair that has both ambiguities in the station's local
 * block, in ascending signal name (a strict total order, for byte-identity), and takes
 * the first with a usable arc, rather than electing one channel and reporting the arc
 * absent when the detector picked the other.  The satellite code-bias columns come
 * from @c MwArcZdMean::code1/code2, the channels the observable actually consumed, so
 * the OSB attribution is not a guess.
 *
 * @par Thread safety
 * Runs inside the parallel produce phase.  It only reads @p est, through the const
 * @c phaseBiasColumn / @c codeBiasColumn lookups (never @c register*, which would
 * race and is illegal after @c beginEpoch()); both lookups use @c map::at and throw
 * on an absent key, so both are wrapped.  It writes only this station's own blocks,
 * @c st.sd_ewl_ctr and @p trace_rows; no shared state is mutated.
 *
 * @param st        This station (provides @c mw_slip, @c sm, @c name, the counters).
 * @param est       Shared-column lookup only (const).
 * @param sta_obs   This station's modeled observations this epoch.
 * @param h2local   Global state index → local column within @p A_r.
 * @param t         Epoch time (trace stamp only).
 * @param[in,out] A_s,A_r,w,y  The station's design blocks, extended in place.
 * @param[in,out] tags  The station's row tags: one @c SdEwl tag per
 *                      appended row (its satellite), in row order.
 * @param[out] trace_rows  Pre-formatted @c network_sd_ewl.csv lines for this
 *                  station-epoch, drained by PASS B's serial consume.  Untouched
 *                  unless @c network.sd_ewl.trace.
 *
 * @par Increment 2b: folding the fixed integer
 * When @c network.sd_ewl.fix_enable published an integer for an arc (@p fix, decided
 * serially before assembly by @ref networkSdEwlAr and read-only here), the row folds
 * @f$\lambda_{EWL}\nabla N^{fixed}@f$ out of the RHS and drops its four local
 * ambiguity coefficients: the local degree of freedom is removed, not constrained.
 * A tight prior on a gauge-adjacent local column would be a stiff prior, not a rank
 * reduction; increment 2 exists so that the local nuisance stops absorbing the row's
 * information and the shared satellite bias receives it.  @c pred_m loses its
 * ambiguity terms in step with the design, because a row whose H changed while its
 * linearization point did not would be folded in a mixed frame.
 *
 * Fixed and float rows coexist at one station, but in separate whitened groups, each
 * with its own pivot and its own common-mode noise.  They cannot share one:
 * @f$R=\mathrm{diag}+\mathrm{var}_p\mathbf{1}\mathbf{1}^\top@f$ is whitened by a dense
 * @f$L^{-1}@f$, which would re-populate a fixed row's zeroed local columns from its
 * float siblings, and the pivot arc's ambiguity columns appear in every row of a
 * group, so a mixed group would both remove and estimate the pivot ambiguity.  Two
 * groups keep both halves exact and make "a fixed row has zero local coefficients"
 * true of the whitened block as well as of H.
 *
 * @param fix  The station table holding the published integers for this epoch
 *             (@c StationState::sd_ewl_fix: constellation, then satellite); this
 *             station's row is read at @p station_index.  @c nullptr, @c network.sd_ewl.fix_enable off, or no
 *             usable entry for an arc => that row takes the increment-1 float path.
 * @param station_index  This station's index into @p fix.
 *
 * A free function (formerly Application::networkAppendSdEwlRows_), so the station
 * steps in the family files can call it; @p config_ is the run's configuration.
 */
void networkAppendSdEwlRows(const GnssConfig& config_, NetStation& st, const NetworkEstimator& est,
                            const std::vector<PppObservation>& sta_obs,
                            const std::map<int, int>& h2local, const GnssTime& t,
                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r, Eigen::VectorXd& w,
                            Eigen::VectorXd& y, std::vector<RowTag>& tags,
                            std::vector<std::string>& trace_rows,
                            const net::StationTable* fix,
                            std::size_t station_index);

// ---- End of the PASS B station steps, declarations ----

/// @brief Run-scope counters of SdEwlAr, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct SdEwlArCounters {

    // --- Run tallies of the SD EWL Route-B integer decision (increment 2a).  Standing
    // invariant #4: on an armed run, `sdewl_dd_fixed == 0` means the decision never
    // fired, which is not the same as "the integers had no effect", so every rejection
    // path below is counted against a denominator.  The 2b row tallies live per
    // station in NetStation::SdEwlCounters (the row build is parallel).
    long long sdewl_epochs = 0;             ///< epochs the decision seam ran (the denominator for everything else).
    long long sdewl_components = 0;         ///< (constellation, connected component) instances processed.
    long long sdewl_dd_pairs_seen = 0;      ///< (station, satellite) arcs admitted to the integer graph.
    long long sdewl_dd_fix_attempts = 0;    ///< DD roundings attempted along spanning-tree / closure edges.
    long long sdewl_dd_fixed = 0;           ///< integers published usable this epoch: measured plus datum values (see sdewl_dd_datum; both are legitimately foldable, but only the measured ones went through a rounding gate).
    long long sdewl_dd_datum = 0;           ///< of those, values assigned as a datum rather than measured: the reference station's own satellites, each station's link satellite, and a satellite framed on first sighting.  A datum value carries no rounding decision, so it cannot be wrong: it defines the frame.
    long long sdewl_dd_reject_frac = 0;     ///< rejected: |fractional residue| over sd_ewl.fix_max_frac_cyc.
    long long sdewl_dd_reject_ffr = 0;      ///< rejected: rounding success probability under sd_ewl.fix_min_ffr.
    long long sdewl_dd_flip = 0;            ///< integer disagreed with the previous epoch on a continuous arc: counted and vetoed (the invariant #1 response).
    long long sdewl_dd_restart = 0;         ///< arc restarted (lagged count fell): memory replaced, no flip charged.
    long long sdewl_sd_ref_reelected = 0;   ///< the sticky component reference station r0 changed (re-datums the whole component's SD frame).
    long long sdewl_dd_skip_channel = 0;    ///< (station, satellite) excluded: its MW arc is on a different tracking-channel pair than the network-common election, so B^s would not cancel in the DD.
    long long sdewl_dd_skip_frame = 0;      ///< (station, satellite) excluded: BIA frame disagrees with the constellation majority.
    long long sdewl_dd_no_link = 0;         ///< (station, satellite) left unfixed: no admissible link satellite reached it from the tree/closure.
    long long sdewl_dd_no_graph = 0;        ///< stations isolated: fewer than two co-visible satellites with any neighbour, so no DD can be formed.
};

} // namespace GnssProcessor
