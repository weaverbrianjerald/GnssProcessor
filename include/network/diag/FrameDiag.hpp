#pragma once
/**
 * @file FrameDiag.hpp
 * @brief Network producer trigger instrumentation and the datum-direction
 *        information ratio.
 *
 * Two measure-only per-epoch diagnostics for explaining why a collapse of `a00`
 * (the degree-0 SH-VTEC coefficient) fires:
 *
 *  - @ref networkShSpectrum: the reduced shared-normal spectrum, with the
 *    smallest direction's eigenvector mass partitioned across the clock /
 *    code-bias / phase-bias / SH-VTEC column families.  The working hypothesis is
 *    that an epoch's re-tap/release pattern momentarily releases a weakly
 *    resisted joint clock↔iono↔field mode; the direct test is whether the smallest
 *    eigenvalue drops and its direction spans both families at the collapse epoch.
 *  - @ref networkPostfitSigma0: a robust post-fit variance factor over the
 *    residuals of every station.  The `sigma0` column of `network_epoch_status.csv`
 *    publishes `sigma0_adapt`, which is computed only inside the within-epoch RAIM
 *    block (`if (within_epoch_armed && carry_warm)`); with that RAIM off it reads
 *    0 and the emit path carries no post-fit residual statistic.
 *
 * Both are observational only: they read the solved system, write CSV rows, and
 * touch no estimator or carry state.  Each is behind its own default-off toggle,
 * so an unarmed run is byte-identical and pays nothing (the function returns on
 * its first line).  Neither toggle is in the warm-resume fingerprint (built in
 * `SnapshotFingerprint.cpp`), so both may be armed on a resume against an untraced
 * snapshot.
 *
 * A third function, @ref networkDatumInfoRatio, runs unconditionally from the
 * rank gate; see its own documentation.
 *
 * Free functions over the run context; they own no state (every counter is a
 * reference into @c ctx), and trace-stream ownership stays in
 * @c network/diag/RunTraces.
 */

#include "common/PppObservation.hpp"
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Core>

#include <memory>
#include <vector>
#include <string>
#include <cstddef>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp (included in the .cpp only)
struct NetworkRunTraces;    // network/diag/RunTraces.hpp (included in the .cpp only)

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp

/**
 * @brief Per-epoch spectrum of the reduced shared normals, with a per-family
 *        eigenvector mass partition of the smallest direction.
 *
 * Runs a full @c SelfAdjointEigenSolver on @c est.reducedSharedNormals() (the
 * symmetric positive-semidefinite Schur complement the rank gate tests, so
 * eigenvalues equal singular values).  For the smallest direction it splits
 * @f$\sum_i |v_i|@f$ over the shared column families identified by
 * @c NetworkEstimator::sharedColumnLabel.  For each of the smallest
 * @c kModesReported directions, the mode-detail file (strided) lists the
 * @c kTopColumns columns with the largest @f$|v_i|@f$, by label.
 *
 * The matrix read is the pre-datum reduced normals: @c solve() applies the datum
 * to a copy, so this is the object the rank gate and the one-shot estimability dump
 * inspect.  Its smallest eigenvalues therefore include the declared gauge freedoms;
 * the trace carries the gate's own absolute @c cutoff
 * (@c rankGateTol · max(λmax,1)) in the last column so @c ev_min can be judged
 * against the threshold that decides rank, not against zero.
 *
 * Cost is O(n_shared³) once per call (about 800³ GPS-only at 305 stations, a few
 * percent of a 41 s epoch).  Call it from the post-solve diagnostic sweep of an
 * emitted epoch only; a failed solve has no meaningful spectrum to report.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces), run.engines.est (est). Read: run.config (config_), run.log
 *            (log).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
 *
 * The bindings of the definition:
 *   - @c ctx = run.ctx: Run context (counters only; nothing is fed back).
 *   - @c config_ = run.config: Live config (reads @c network.trace_sh_spectrum_stride).
 *   - @c log = run.log: Main logger (warns once if the eigensolve fails).
 *   - @c traces = run.traces: Owns @c network_sh_spectrum.csv + the mode detail file.
 *   - @c se = ep.se: This epoch (week/tow stamp).
 *   - @c est = run.engines.est: Estimator; non-const because @c reducedSharedNormals lazily
 *     reduces (idempotent; the reduction has already happened by the post-solve call site, so
 *     this is a cache read).
 */
void networkShSpectrum(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Measure-only robust post-fit variance factor over every station.
 *
 * For each station in the solve, reconstructs its local parameters and forms
 * @f$v = y - A_s x_s - A_r x_r@f$, then pools @f$|v_i|\sqrt{w_i}@f$: the same
 * measurement-standardized residual the within-epoch RAIM pools, but over all
 * rows instead of only the carried-ambiguity phase rows.  Reports
 * @f$\hat\sigma_0 = 1.4826\,\mathrm{median}@f$ of that pool, plus separate code
 * and phase pools.
 *
 * Row classification: each row's class comes from its contribution
 * tag (@c StationAssembly::row_tags, read through @ref rowClassOfKind in
 * network/assembly/RowClassify), not from the columns it touches:
 *  - an SH-VTEC to slant-ionosphere tie row is excluded from all three pools (about
 *    70 to 90 % of those rows exceed 3σ by construction, because the single-layer SH
 *    surface cannot represent per-LOS slant structure, so pooling them would swamp
 *    the statistic with a known artifact);
 *  - a coupling row or a whitened carried-prior block row is also excluded (it is a
 *    statement about the carry, not about data);
 *  - an observation row is code or phase by its declared observation type (the
 *    companion code and second phase channels included);
 *  - any other row (a pivot pin, a local prior or anchor row, an unclassified row)
 *    enters only the pooled @c sigma0_pf and is counted by the rows extension as
 *    unattributed.
 *
 * Interpretation: read @c sigma0_pf_code, not @c sigma0_pf.  With free
 * per-(station,satellite,signal) ambiguities (the configuration whenever
 * @c network.phase_leveling is off) each phase row is fitted exactly by its own
 * ambiguity: the phase residuals are identically zero, so @c sigma0_pf_phase is 0
 * by construction and the pooled @c sigma0_pf is diluted by those zeros roughly in
 * proportion to the phase/code row ratio.  The zero is itself informative: the
 * phase rows carry no redundancy and therefore no fault-detection power in this
 * configuration, which is why the producer's phase-side QC (RAIM) is gated on
 * phase leveling.  The run-end line reports the per-pool row counts so the
 * dilution is explicit.
 *
 * Results land in @c ctx.carry.ep_sigma0_pf* / @c ep_pf_rows for the epoch-status
 * row; @c ep_pf_rows == 0 means the pass produced nothing (blank columns), and
 * @c ctx.counters.pf_epochs_total is the run-end counter that shows whether the
 * wiring ever fired.
 *
 * Keyed extension (@c network.trace_postfit_keyed): the
 * same pass optionally pools per station, per satellite and per signal (the row's
 * satellite and observation code, read from its tag) and writes one
 * @c network_pf_keyed.csv row per key per epoch.  These keys are the axes that
 * separate candidate sources of the misfit (station
 * geography ⇒ iono front, per-satellite structure ⇒ orbit/clock input, per-signal
 * split ⇒ dispersive).  With the keyed trace closed the pass builds no key.
 *
 * Rows extension (@c network.trace_postfit_rows): the
 * same pass writes every observation row as one signed line of
 * @c network_pf_rows.csv.  @c resid_m is @f$v_i@f$ itself (observed − predicted,
 * metres, the sign convention of @f$v = y - A_s x_s - A_r x_r@f$ above), @c w the
 * row's information weight, @c elev_deg the LOS elevation read off @c modeled.
 * The keyed pools identify which station / satellite / signal; the rows give the
 * residual of one LOS: its mean (a constant no state absorbed), its dispersive
 * ratio across signals, its elevation shape.  The satellite and the signal written
 * are the row tag's (the observation's own code), so a code row whose only
 * shared column is the satellite clock (a code signal tracked at one station, or a
 * posture with the satellite code biases supplied externally) keeps its signal and
 * joins the per-signal pool.  Rows that are not observations are counted
 * (@c ctx.counters.pf_rows_unattributed), not written; there is no cost when the rows
 * trace is closed.
 *
 * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log), run.setup.stations
 *            (stations), run.engines.est (est).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se), ep.solve.x_s
 *           (x_s), ep.assembly.asm_res (asm_res), ep.assembly.est_index_of (est_index_of),
 *           &ep.pass_a.modeled (modeled).
 *
 * The bindings of the definition:
 *   - @c ctx = run.ctx: Run context (per-epoch results + run counters).
 *   - @c config_ = run.config: Live config; not read (the toggles act through which traces are
 *     open).
 *   - @c log = run.log: Main logger.
 *   - @c traces = run.traces: Owns @c network_pf_keyed.csv and @c network_pf_rows.csv (a
 *     closed trace turns its extension off).
 *   - @c se = ep.se: This epoch (week/tow stamp for the keyed rows).
 *   - @c stations = run.setup.stations: Loaded network stations (names key the STA rows; size
 *     bounds the loop over @c asm_res).
 *   - @c est = run.engines.est: Estimator (reconstructStation).
 *   - @c x_s = ep.solve.x_s: Solved shared vector.
 *   - @c asm_res = ep.assembly.asm_res: Per-station assembly blocks.
 *   - @c est_index_of = ep.assembly.est_index_of: Station index -> estimator index (<0 = not
 *     in the solve).
 *   - @c modeled = &ep.pass_a.modeled: Per-station modelled observation set of this epoch (the
 *     PASS-A output the driver holds), read only by the rows extension for the LOS elevation
 *     of each written row. nullptr ⇒ every @c elev_deg is empty.
 */
void networkPostfitSigma0(NetworkRun& run, const NetworkEpoch& ep);

/**
 * @brief Per-epoch datum-direction information ratio
 *        @f$R=(v^{\mathsf T}Nv)/(v^{\mathsf T}v)\,/\,\text{cutoff}@f$ over the declared
 *        datum directions: the counter that reports a carry filling the null space.
 *
 * The rank gate collapses the whole spectrum to an integer.  A declared gauge that
 * acquires information moves the observed count by exactly one, and the run log
 * says only "observed 2, declared 3", the same message a mis-calibrated tolerance
 * produces.  A filled gauge direction can rise several decades above the cutoff in
 * one epoch with an over-specified withhold as the only visible symptom.
 *
 * @f$R\ll 1@f$ ⇒ the direction is still gauge (the datum owns it).  @f$R\ge 1@f$ ⇒
 * something folded before the gate (a carry, a coupling row, a prior) is pinning a
 * direction the datum is supposed to own, and the gate is about to miscount it.
 * The run-end gate advice branches on this ratio, because a genuine
 * mis-calibration leaves @f$R@f$ near zero while a filled null shows it jumping
 * decades at the offending epoch.
 *
 * Cost is one @f$k\times k@f$ quadratic form per constellation (k = that
 * constellation's clock count, ~10-30) against the @f$O(n^3)@f$ eigensolve the gate
 * already pays, so this runs unconditionally, on purpose: behind a default-off knob
 * it would be absent from the runs that need it.  It is measure-only and touches no
 * estimator state, so it is fingerprint-safe and legal on a warm resume.
 *
 * Directions are built analytically (the normalized indicator over a
 * constellation's registered clock columns) and not from an eigensolve:
 * @f$v^{\mathsf T}Nv@f$ needs no eigenvector, and the analytic direction is what
 * @c NetworkSbasis::buildSharedDatum declares.
 *
 * @param ctx          Run context (counters written; nothing else touched).
 * @param traces       Event sink (per-epoch breach and clear edges).
 * @param se           Epoch (time stamps the events).
 * @param est          Estimator: @c reducedSharedNormals() and @c clockColumn().
 * @param declared_dim @c Plan::declared_dimension for this epoch (<= 0 ⇒ no-op).
 * @param registered_clock_sats Satellites with a live clock column this epoch.
 * @param ev_max       Largest eigenvalue from the gate (<0 ⇒ the gate computed no
 *                     spectrum on this exit, and this function is a no-op).
 * @param gate_over_withheld True when this epoch was withheld as over-specified; the
 *                     coincidence counter that makes the run-end advice actionable.
 * @param N_gate       The normals the gate judged when it did not judge
 *                     @c est.reducedSharedNormals(): the view with the clock
 *                     clones marginalized (network/carry/ClockClone), over the non-clone
 *                     columns, whose indices are those of the layout.  Null (the default)
 *                     reads the reduced normals.
 */
void networkDatumInfoRatio(NetworkRunContext& ctx, NetworkRunTraces& traces,
                           const SyncedEpoch& se, NetworkEstimator& est,
                           int declared_dim,
                           const std::vector<std::string>& registered_clock_sats,
                           double ev_max, bool gate_over_withheld,
                           const Eigen::MatrixXd* N_gate = nullptr);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: frame diagnostics: the post-fit sigma0 and rows, the shared-normal
 *        spectrum trace.
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in FrameDiag.cpp, print in this order, each gated on its own knob:
 *   - networkPostfitRunEnd: the post-fit sigma0 and the post-fit rows.
 *   - networkShSpectrumRunEnd: the shared-normal spectrum trace.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkFrameDiagRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of FrameDiag, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct FrameDiagCounters {

    // Datum-direction information ratio R = (vᵀNv/vᵀv)/cutoff: one sample per
    // (epoch × declared datum direction), on the same reduced normals
    // and the same cutoff the rank gate judges by.  The gate collapses that spectrum to
    // an integer, so a declared gauge that acquires information moves the observed count
    // by one and reads like a mis-calibrated tolerance.  A coupling row with a
    // truncated family sum can raise one clock gauge decades above the cutoff in a
    // single epoch, with over-specified withholds as the only symptom.  R ≪ 1 = still a
    // gauge; R ≥ 1 = something folded before the gate is pinning a direction the datum
    // owns.  Computed unconditionally (one k×k quadratic form per constellation, against
    // the gate's own O(n³) eigensolve), because a knob-gated version would be dark on
    // the runs that need it.
    std::size_t ddi_epochs        = 0;   ///< epochs the ratio was evaluated (denominator)
    std::size_t ddi_dirs          = 0;   ///< (epoch × direction) samples
    std::size_t ddi_breach_epochs = 0;   ///< epochs with >= 1 direction at R >= 1
    std::size_t ddi_breach_edges  = 0;   ///< breach and clear edges emitted
    std::size_t ddi_over_breach   = 0;   ///< over-specified withholds coincident with a breach: the discriminator the run-end advice branches on
    double      ddi_ratio_max     = -1.0;///< largest R over the run (-1 = never evaluated)
    double      ddi_ratio_max_tow = 0.0; ///< ...its epoch
    std::string ddi_ratio_max_dir;       ///< ...its identity ("CLK|G")
    double      ddi_ratio_min     = -1.0;///< smallest R: the floor at which a free datum sits

    // Postfit sigma0 run-end counter and warn latch (networkPostfitSigma0).
    long long pf_epochs_total    = 0;   ///< run-end counter (0 means inert)
    bool      pf_empty_logged    = false;  ///< one-shot "armed but pooled nothing" warn latch
    // Run-end accounting of the signed per-row dump (network.trace_postfit_rows):
    // rows written (code and phase), written rows with no elevation, and data rows the
    // pass visited but could attribute to neither family (not written).  All are
    // needed: "0 written under an armed trace" must be visible, and a non-zero
    // unattributed count says which rows the dump does not carry.
    long long pf_rows_written      = 0;
    long long pf_rows_code         = 0;   ///< …of which kind=code
    long long pf_rows_phase        = 0;   ///< …of which kind=phase
    long long pf_rows_no_elev      = 0;   ///< …written with an empty elev_deg (station absent from the modeled set, or no satellite on the row)
    long long pf_rows_unattributed = 0;   ///< rows attributed to neither family (the row tag is not an observation: pins, local prior and anchor rows, unclassified rows); counted, not written
    long long pf_rows_no_signal    = 0;   ///< rows written with an empty signal (the row tag named no signal); 0 expected
    // --- Spectrum bookkeeping ---
    long long sh_spectrum_epochs      = 0;      ///< traced epochs (also drives the mode-detail stride)
    bool      sh_spectrum_fail_logged = false;  ///< one-shot eigensolve-failure warn latch
};

}  // namespace GnssProcessor
