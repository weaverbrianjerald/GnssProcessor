#pragma once
/**
 * @file NetworkFrameDiag.hpp
 * @brief Network producer trigger instrumentation (SPEC_day129_frame_guards Phase 1)
 *        and the datum-direction information ratio (invariant #7).
 *
 * Two measure-only per-epoch diagnostics that the DOY-129 investigation
 * (`docs/SPEC_day129_frame_guards.md Part A`) needed to explain why an `a00`
 * collapse fires:
 *
 *  - @ref networkShSpectrum (P1-3): the reduced shared-normal spectrum, with the
 *    smallest direction's eigenvector mass partitioned across the clock /
 *    code-bias / phase-bias / SH-VTEC column families.  The working hypothesis of
 *    §3.2 is that an epoch's re-tap/release pattern momentarily releases a weakly
 *    resisted joint clock↔iono↔field mode; the direct test is whether the smallest
 *    eigenvalue drops and its direction spans both families at the collapse epoch.
 *  - @ref networkPostfitSigma0 (P1-4): a robust post-fit variance factor over the
 *    residuals of every station.  The `sigma0` column of `network_epoch_status.csv`
 *    publishes `sigma0_adapt`, which is computed only inside the within-epoch RAIM
 *    block (`if (within_epoch_armed && carry_warm)`); with that RAIM off it reads
 *    0 and the emit path carries no post-fit residual statistic.
 *
 * Both are observational only: they read the solved system, write CSV rows, and
 * touch no estimator or carry state.  Each is behind its own default-off toggle,
 * so an unarmed run is byte-identical and pays nothing (the function is not
 * called).  Neither toggle is in the warm-resume fingerprint (built in
 * `NetworkRunContext.cpp`), so both may be armed on a resume against an untraced
 * snapshot.
 *
 * A third function, @ref networkDatumInfoRatio, runs unconditionally from the
 * solve glue; see its own documentation.
 *
 * Seam contract per CLAUDE.md: free functions over the run context; they own no
 * state (every counter is a reference into @c ctx), and trace-stream ownership
 * stays in @c logging/NetworkRunTraces.
 */

#include "common/PppObservation.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Core>

#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp (included in the .cpp only)
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp (included in the .cpp only)

/**
 * @brief Per-epoch spectrum of the reduced shared normals, with a per-family
 *        eigenvector mass partition of the smallest direction (P1-3).
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
 * @param ctx      Run context (counters only; nothing is fed back).
 * @param config_  Live config (reads @c network.trace_sh_spectrum_stride).
 * @param log      Main logger (warns once if the eigensolve fails).
 * @param traces   Owns @c network_sh_spectrum.csv + the mode detail file.
 * @param se       This epoch (week/tow stamp).
 * @param est      Estimator; non-const because @c reducedSharedNormals lazily
 *                 reduces (idempotent; the reduction has already happened by the
 *                 post-solve call site, so this is a cache read).
 */
void networkShSpectrum(NetworkRunContext& ctx, const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       NetworkRunTraces& traces, const SyncedEpoch& se,
                       NetworkEstimator& est);

/**
 * @brief Measure-only robust post-fit variance factor over every station (P1-4).
 *
 * For each station in the solve, reconstructs its local parameters and forms
 * @f$v = y - A_s x_s - A_r x_r@f$, then pools @f$|v_i|\sqrt{w_i}@f$: the same
 * measurement-standardized residual the within-epoch RAIM pools, but over all
 * rows instead of only the carried-ambiguity phase rows.  Reports
 * @f$\hat\sigma_0 = 1.4826\,\mathrm{median}@f$ of that pool, plus separate code
 * and phase pools.
 *
 * Row classification (each row is assigned once, in this order):
 *  - a row with any nonzero in a shared VtecSh column is an SH↔slant-iono
 *    pseudo-observation and is excluded from all three pools (about 70–90 % of
 *    those rows exceed 3σ by construction, documented in
 *    @c NetworkIonoDeweightGate.hpp, so pooling them would swamp the statistic
 *    with a known artifact);
 *  - a row at or past @c cpl_row_start is a Card-1 coupling pseudo-observation
 *    and is also excluded (it is a statement about the carry, not about data);
 *  - a row touching a satellite phase-bias (@c PB:) column or a present-ambiguity
 *    local column is phase;
 *  - a row that is not phase and touches a satellite code-bias (@c CB:) or clock
 *    (@c CLK:) column is code;
 *  - any other data row (no shared partial: pivot pins, local prior rows) enters
 *    only the pooled @c sigma0_pf and is counted by the rows extension as
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
 * @c ctx.carry.pf_epochs_total is the run-end counter that shows whether the
 * wiring ever fired (standing invariant #4).
 *
 * Keyed extension (§4.3 RHS localization, @c network.trace_postfit_keyed): the
 * same pass optionally pools per station, per satellite and per signal (the row's
 * sat/signal read from the shared CLK/CB/PB column labels it touches) and writes
 * one @c network_pf_keyed.csv row per key per epoch.  The Phase-1 verdict put the
 * collapse trigger on the right-hand side with the misfit global across stations;
 * these keys are the axes that separate the remaining candidate sources (station
 * geography ⇒ iono front, per-satellite structure ⇒ orbit/clock input, per-signal
 * split ⇒ dispersive).  With the keyed trace closed there are no extra label
 * lookups and the off path is unchanged.
 *
 * Rows extension (SPEC_event_harness §24.6, @c network.trace_postfit_rows): the
 * same pass writes every data row it attributes to a family as one signed line of
 * @c network_pf_rows.csv.  @c resid_m is @f$v_i@f$ itself (observed − predicted,
 * metres, the sign convention of @f$v = y - A_s x_s - A_r x_r@f$ above), @c w the
 * row's information weight, @c elev_deg the LOS elevation read off @p modeled.
 * The keyed pools identify which station / satellite / signal; the rows give the
 * residual of one LOS: its mean (a constant no state absorbed), its dispersive
 * ratio across signals, its elevation shape.  The signal written is the
 * observation's own (the assembly's @c row_sig), falling back to the first
 * labelled CB/PB column's signal (card OWD).  Rows of neither family are counted
 * (@c ctx.carry.pf_rows_unattributed), not written.  The label lookups are shared
 * with the keyed path; there is no cost when the rows trace is closed.
 *
 * Code-side clock fallback (card ZP; found by card PV).  A satellite code-bias
 * column is not present in every posture: with the satellite biases supplied
 * externally (a .BIA product, "0 code biases") there are no @c CB: columns, and
 * with them estimated the W6 datum signal's OSB is the gauge and carries no
 * column either (§24.9 records this case).  Without a fallback those code rows
 * fall to @c pf_rows_unattributed and an external-bias run's wire census sees no
 * code arcs.  The code side therefore carries the counterpart of the phase side's
 * ambiguity-column touch: a data row that is not phase and touches a satellite
 * clock (@c CLK:) column is a code row, because every observation equation
 * carries the satellite clock.  Rows with no shared partial (pivot pins, local
 * prior rows) touch neither and stay unattributed, which is what that counter
 * means.  Remaining limitation: such a row has no @c CB: / @c PB: label, so in the
 * keyed pools its signal is empty; it joins the per-station and per-satellite
 * pools but not the per-signal one.  The receiver code-bias local column does not
 * help: on the FT63 lineage it is one per-constellation @c GPS_DCB state, not a
 * per-signal one.  The same gap is present in @c filter/NetworkRowClassify's
 * @c classifyRow (@c if(cb) ObsCode; else ObsOther), which the prefit census, the
 * seed tripwire, the topology ledger and the structural FUSE share; closing it
 * there is solve-affecting whenever @c network.prefit_fuse_sigma is armed, so it
 * needs its own card.
 *
 * @param ctx           Run context (per-epoch results + run counters).
 * @param config_       Live config; not read (the toggles act through which traces
 *                      are open).
 * @param log           Main logger.
 * @param traces        Owns @c network_pf_keyed.csv and @c network_pf_rows.csv
 *                      (a closed trace turns its extension off).
 * @param se            This epoch (week/tow stamp for the keyed rows).
 * @param stations      Loaded network stations (names key the STA rows; size
 *                      bounds the loop over @p asm_res).
 * @param est           Estimator (reconstructStation).
 * @param x_s           Solved shared vector.
 * @param asm_res       Per-station assembly blocks.
 * @param est_index_of  Station index -> estimator index (<0 = not in the solve).
 * @param modeled       Per-station modelled observation set of this epoch (the
 *                      PASS-A output the driver holds), read only by the rows
 *                      extension for the LOS elevation of each written row.
 *                      nullptr ⇒ every @c elev_deg is empty.
 */
void networkPostfitSigma0(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of,
                          const std::vector<std::vector<PppObservation>>* modeled = nullptr);

/**
 * @brief Per-epoch datum-direction information ratio
 *        @f$R=(v^{\mathsf T}Nv)/(v^{\mathsf T}v)\,/\,\text{cutoff}@f$ over the declared
 *        datum directions (standing invariant #7): the counter that reports a carry
 *        filling the null space.
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
 * @param traces       Event sink (per-epoch breach/clear edges, invariant #7's two edges).
 * @param se           Epoch (time stamps the events).
 * @param est          Estimator: @c reducedSharedNormals() and @c clockColumn().
 * @param declared_dim @c Plan::declared_dimension for this epoch (<= 0 ⇒ no-op).
 * @param registered_clock_sats Satellites with a live clock column this epoch.
 * @param ev_max       Largest eigenvalue from the gate (<0 ⇒ the gate computed no
 *                     spectrum on this exit, and this function is a no-op).
 * @param gate_over_withheld True when this epoch was withheld as over-specified; the
 *                     coincidence counter that makes the run-end advice actionable.
 */
void networkDatumInfoRatio(NetworkRunContext& ctx, NetworkRunTraces& traces,
                           const SyncedEpoch& se, NetworkEstimator& est,
                           int declared_dim,
                           const std::vector<std::string>& registered_clock_sats,
                           double ev_max, bool gate_over_withheld);

}  // namespace GnssProcessor
