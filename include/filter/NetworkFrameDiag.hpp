#pragma once
/**
 * @file NetworkFrameDiag.hpp
 * @brief Network producer trigger instrumentation — SPEC_day129_frame_guards Phase 1.
 *
 * Two measure-only per-epoch diagnostics that the DOY-129 investigation
 * (`docs/SPEC_day129_frame_guards.md Part A`) found MISSING when it tried
 * to answer why an `a00` collapse fires:
 *
 *  - @ref networkShSpectrum (P1-3) — the reduced shared-normal spectrum with
 *    each small direction's eigenvector mass partitioned across the clock /
 *    code-bias / phase-bias / SH-VTEC column families.  §3.2's working
 *    hypothesis is that an epoch's re-tap/release pattern momentarily releases a
 *    weakly-resisted JOINT clock↔iono↔field mode; the direct test is whether the
 *    smallest eigenvalue craters AND its direction spans both families at the
 *    collapse epoch.
 *  - @ref networkPostfitSigma0 (P1-4) — a robust post-fit variance factor over
 *    EVERY station's residuals.  The `sigma0` column of
 *    `network_epoch_status.csv` reads 0.0000 on all 2160 rows of the reference
 *    run because it publishes `sigma0_adapt`, computed only inside the
 *    within-epoch RAIM block (`if (within_epoch_armed && carry_warm)`); with
 *    that RAIM off, the emit path carries no post-fit residual statistic at all.
 *
 * Both are strictly observational: they read the solved system, write CSV rows,
 * and touch no estimator or carry state.  Each is behind its own default-OFF
 * toggle, so an unarmed run is byte-identical and pays nothing (neither function
 * is called).  Neither toggle is in the warm-resume fingerprint
 * (`NetworkRunContext.cpp:172-307`), so both may be armed on a resume against an
 * untraced snapshot.
 *
 * Seam contract per CLAUDE.md: free functions over the run context; they own no
 * state (every counter is a reference into @c ctx), and trace-stream ownership
 * stays in @c logging/NetworkRunTraces.
 */

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "rinex/MultiRinexSync.hpp"

#include <Eigen/Core>

#include <memory>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief P1-3: per-epoch spectrum of the reduced shared normals + per-family
 *        eigenvector mass partition of the smallest directions.
 *
 * Runs a full @c SelfAdjointEigenSolver on @c est.reducedSharedNormals() (the
 * same symmetric positive-SEMIdefinite Schur complement the rank gate tests, so
 * eigenvalues equal singular values), then for each of the smallest
 * @c kModesReported directions splits @f$\sum_i |v_i|@f$ over the shared column
 * families identified by @c NetworkEstimator::sharedColumnLabel.
 *
 * The matrix read is the PRE-datum reduced normals — @c solve() pins a COPY
 * (NetworkEstimator.cpp:255), so this is exactly the object the rank gate and the
 * one-shot estimability dump inspect.  Its smallest eigenvalues therefore include
 * the DECLARED gauge freedoms; the trace carries the gate's own absolute @c cutoff
 * (@c rankGateTol · max(λmax,1)) in the last column so @c ev_min can be judged
 * against the threshold that actually decides rank, not against zero.
 *
 * Cost is O(n_shared³) once per call — ~800³ GPS-only at 305 stations, a few
 * percent of a 41 s epoch.  Call it from the post-solve diagnostic sweep of an
 * EMITTED epoch only (a failed solve has no meaningful spectrum to report).
 *
 * @param ctx      Run context (counters only; nothing is fed back).
 * @param config_  Live config (reads @c network.trace_sh_spectrum_stride).
 * @param log      Main logger (warns once if the eigensolve fails).
 * @param traces   Owns @c network_sh_spectrum.csv + the mode detail file.
 * @param se       This epoch (week/tow stamp).
 * @param est      Estimator — non-const because @c reducedSharedNormals lazily
 *                 reduces (idempotent; the reduction has already happened by the
 *                 post-solve call site, so this is a cache read).
 */
void networkShSpectrum(NetworkRunContext& ctx, const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       NetworkRunTraces& traces, const SyncedEpoch& se,
                       NetworkEstimator& est);

/**
 * @brief P1-4: measure-only robust post-fit variance factor over every station.
 *
 * For each station in the solve, reconstructs its local parameters and forms
 * @f$v = y - A_s x_s - A_r x_r@f$, then pools @f$|v_i|\sqrt{w_i}@f$ — the same
 * measurement-standardized residual the within-epoch RAIM pools, but over ALL
 * rows rather than only the carried-ambiguity phase rows.  Reports
 * @f$\hat\sigma_0 = 1.4826\,\mathrm{median}@f$ of that pool, plus separate CODE
 * and PHASE pools.
 *
 * Row classification (each row is assigned exactly once):
 *  - a row with any nonzero in a shared VtecSh column is an SH↔slant-iono
 *    PSEUDO-observation — excluded from all three pools (69–90 % of those rows
 *    exceed 3σ by construction, documented in @c NetworkIonoDeweightGate.hpp,
 *    so pooling them would swamp the statistic with a known artifact);
 *  - a row at or past @c cpl_row_start is a Card-1 coupling pseudo-observation —
 *    also excluded (it is a statement about the carry, not about data);
 *  - otherwise a row touching any present-ambiguity local column is PHASE;
 *  - every remaining row is CODE.
 *
 * INTERPRETATION — read @c sigma0_pf_code, not @c sigma0_pf.  With free
 * per-(station,satellite,signal) ambiguities (the configuration whenever
 * @c network.phase_leveling is off) each phase row is fitted EXACTLY by its own
 * ambiguity: the phase residuals are identically zero, so @c sigma0_pf_phase is
 * 0 by construction and the pooled @c sigma0_pf is diluted by those zeros
 * roughly in proportion to the phase/code row ratio.  That zero is itself the
 * finding — it says the phase rows carry no redundancy and therefore no
 * fault-detection power in this configuration, which is why the producer's
 * phase-side QC (RAIM) is gated on phase-leveling in the first place.  The run
 * end line reports the per-pool row counts so the dilution is explicit.
 *
 * Results land in @c ctx.carry.ep_sigma0_pf* / @c ep_pf_rows for the
 * epoch-status row; @c ep_pf_rows == 0 means the pass produced nothing (blank
 * columns) and @c ctx.carry.pf_epochs_total is the run-end "did this wiring ever
 * fire" counter (standing invariant #4).
 *
 * KEYED EXTENSION (§4.3 RHS localization, @c network.trace_postfit_keyed): the
 * same pass optionally pools per STATION, per SATELLITE, and per SIGNAL (the
 * row's sat/signal read from the shared CLK/CB/PB column labels it touches) and
 * writes one @c network_pf_keyed.csv row per key per epoch.  The Phase-1
 * verdict put the collapse trigger on the RIGHT-hand side with the misfit
 * global across stations; these keys are the axes that discriminate the
 * remaining candidate sources (station geography ⇒ iono front, per-satellite
 * structure ⇒ orbit/clock input, per-signal split ⇒ dispersive).  Zero extra
 * label lookups when the keyed trace is closed — the OFF path is unchanged.
 *
 * @param ctx           Run context (per-epoch results + run counters).
 * @param config_       Live config (only the trace toggles are read).
 * @param log           Main logger.
 * @param traces        Owns @c network_pf_keyed.csv (closed ⇒ keyed path off).
 * @param se            This epoch (week/tow stamp for the keyed rows).
 * @param stations      Loaded network stations (names key the STA rows; size
 *                      bounds the loop over @p asm_res).
 * @param est           Estimator (reconstructStation).
 * @param x_s           Solved shared vector.
 * @param asm_res       Per-station assembly blocks.
 * @param est_index_of  Station index -> estimator index (<0 = not in the solve).
 */
void networkPostfitSigma0(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of);

/**
 * @brief STANDING INVARIANT #7: the per-epoch DATUM-DIRECTION INFORMATION RATIO
 *        @f$R=(v^{\mathsf T}Nv)/(v^{\mathsf T}v)\,/\,\text{cutoff}@f$ over the DECLARED
 *        datum directions — the counter that stops a carry from silently eating the
 *        null space.
 *
 * The rank gate collapses the whole spectrum to an integer.  A declared gauge that
 * ACQUIRES information therefore moves the observed count by exactly one, and the run
 * log says only "observed 2, declared 3" — the same sentence a mis-calibrated tolerance
 * produces.  Measured on simnet24 at TOW 305220: seven departed clock columns drove one
 * constellation's gauge direction from 1e-11 to 9.43e-3 in a single epoch, five decades
 * over the cutoff, and the only visible symptom was four OVER-specified withholds.
 *
 * @f$R\ll 1@f$ ⇒ the direction is still gauge (the datum owns it).  @f$R\ge 1@f$ ⇒
 * something folded PRE-gate is pinning a direction the datum is supposed to own — a
 * carry, a coupling row, a prior — and the gate is about to miscount it.  The ratio is
 * the discriminator the run-end gate advice branches on, because a genuine
 * mis-calibration leaves @f$R@f$ near zero while a filled null shows it jumping decades
 * at the offending epoch.
 *
 * Cost is one @f$k\times k@f$ quadratic form per constellation (k = that constellation's
 * clock count, ~10-30) against the @f$O(n^3)@f$ eigensolve the gate already pays — so
 * this runs UNCONDITIONALLY.  That is deliberate and is the whole point of the
 * invariant: a diagnostic behind a default-OFF knob would have been dark on exactly the
 * runs that needed it.  It is measure-only and touches no estimator state, so it is
 * fingerprint-SAFE and legal on a warm resume.
 *
 * Directions are built ANALYTICALLY (the normalized indicator over a constellation's
 * registered clock columns) rather than from an eigensolve: @f$v^{\mathsf T}Nv@f$ needs
 * no eigenvector, and the analytic direction is exactly what
 * @c NetworkSbasis::buildSharedDatum declares.
 *
 * @param ctx          Run context (counters written; nothing else touched).
 * @param traces       Event sink (per-epoch breach/clear edges — invariant #7's two edges).
 * @param se           Epoch (time stamps the events).
 * @param est          Estimator — @c reducedSharedNormals() and @c clockColumn().
 * @param declared_dim @c Plan::declared_dimension for this epoch.
 * @param registered_clock_sats Satellites with a live clock column this epoch.
 * @param ev_max       Largest eigenvalue from the gate (<0 ⇒ the gate computed no
 *                     spectrum on this exit, and this function is a no-op).
 * @param gate_over_withheld True when THIS epoch was withheld OVER-specified — the
 *                     coincidence counter that makes the run-end advice actionable.
 */
void networkDatumInfoRatio(NetworkRunContext& ctx, NetworkRunTraces& traces,
                           const SyncedEpoch& se, NetworkEstimator& est,
                           int declared_dim,
                           const std::vector<std::string>& registered_clock_sats,
                           double ev_max, bool gate_over_withheld);

}  // namespace GnssProcessor
