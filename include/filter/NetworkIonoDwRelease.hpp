/**
 * @file NetworkIonoDwRelease.hpp
 * @brief Iono-deweight release hygiene: reseed the slant states a zero-weight era
 *        left unvalidated, bound what such an era can park, and screen the tie rows
 *        that a release or a first admission couples to the SH field.
 *        Card: docs/SPEC_deweight_release_hygiene.md.
 *
 * ## The defect
 *
 * The station-adaptive SH-VTEC-constraint deweight gate
 * (@ref GnssProcessor::NetworkIonoDeweightGate) can drive a station's weight
 * @f$w_s@f$ to zero, at which point the assembly appends no SH↔slant tie rows for
 * it (ApplicationNetworkAssembly.cpp, @c networkAppendShIonoRows_ returns early on
 * @f$w_s \le 0@f$).  That decouple is intended, but it leaves the station's per-LOS
 * slant-iono states unconstrained by the field for the whole era, while the storm
 * that triggered the gate makes them least trustworthy.  The network producer is a
 * per-epoch absolute solve whose per-station StateManager holds a frozen
 * linearization point (`setStateVector` at setup, ApplicationNetworkSetup.cpp), so
 * a bad slant/ambiguity pair does not decay: it parks and presents a bit-identical
 * row every epoch.  The local block does not flag it, because the station's own
 * nuisances absorb it exactly.
 *
 * When the gate releases, the tie rows return against those parked states with no
 * event of their own.
 *
 * ## The coupling edge at the measured event
 *
 * The event that motivated the card (DOY-129 TOW 329160) was not a deweight release.
 * The station concerned stayed at @f$w_s = 1@f$ throughout, and its only @c IONO_DW
 * event is an engage at the break, i.e. the gate reacting to it.  The tie row that
 * coupled the parked slant to the field was a satellite crossing the
 * `elevation >= iw.elevation_cutoff_deg` / `h2local` admission gate and entering
 * the tie stack for the first time.  Two reading traps apply to that diagnosis:
 * the `n_dw_zero` / `dw_min_w` figures are network aggregates from
 * @ref NetworkIonoDeweightGate::aggregate, not per-station values; and the `ep_dw_*`
 * epoch-status harvest is taken after `advance()`, so the row stamped at epoch N
 * describes the weights epoch N+1 uses.
 *
 * The defect class is the same in both cases: a slant column carrying an
 * unvalidated parked value is tied to the SH field and discharges into
 * @f$a_{00}@f$.  The row itself is identical across the edge (same @f$w@f$, same
 * @f$y@f$); only its coupling to the SH block through the Schur reduction changes.
 * INC-1 and INC-2 key on the gate weight and so cannot act on an admission edge;
 * they remain the guard for a genuine release.  INC-3b screens the admission edge,
 * which is why this seam screens admission as well as release.  The event was
 * attributed with the reduced-RHS class-attribution instrument
 * @c network.rhs_attrib_tow; the measurements are in the card.
 *
 * Standing invariant #1 covers this class: a zero-weight era makes the slant state
 * time-linked to nothing and therefore held, and the release (or first admission)
 * is its event.  This seam is the wired response.
 *
 * ## What this seam does (three independently configured increments)
 *
 *  - **INC-1 `network.iono_dw_release_reseed`** (bool): when a station's weight
 *    rises above `network.iono_dw_release_floor_eps`, three things happen for that
 *    station, so the re-engaged ties fold information and not the parked values:
 *      * every per-LOS slant-iono state is reseeded from the current SH field
 *        evaluation (a state write, @c st.sm->setStateVector, plus a variance
 *        re-inflation).  This differs from erasing the deviation stash: the stash
 *        only sets the row's predicted deviation to zero, a weak assertion that the
 *        per-constellation common-mode block (σ 50 m off-diagonal) deliberately
 *        leaves unenforced on the level, so the parked metres would survive it.
 *      * every float ambiguity of the station is refloated through the standard
 *        slip funnel, deferred one epoch (see the ordering contract): the drain
 *        puts it into `carry_slipped_raw`, whose union carries it into
 *        `carry_slipped_ambs`, and marks it with @c markAmbiguityPendingReseed.
 *        The raw subset is load-bearing: the Iono-2 v3k `d5_keep` policy keeps a
 *        deviation level for a non-raw re-tap, which would restore the parked
 *        value this response discards.  The ambiguity re-seed reads the current
 *        iono state (@c StateManager::computeAmbiguitySeed takes the state vector),
 *        so the slant reseed must land first; it does, one epoch earlier.
 *      * the station's per-LOS deviation stash entries are erased.
 *  - **INC-2 `network.iono_dw_park_bound_tecu`** (double, 0 = off): a LOS whose
 *    absolute slant (the SH field plus the harvested deviation) exceeds the bound is
 *    reset to the field evaluation immediately, without waiting for a release.
 *    Applied while the station is decoupled (weight ≤ eps) and also while the gate
 *    has no history for it (`samples() == 0`).  The second case covers a warm
 *    resume: no gate state is serialized, so a resumed leg starts every station at
 *    @f$w=1@f$ and re-folds every tie row on its first assembly with no release edge
 *    to hook.  The two triggers are counted separately.  The bound is in slant
 *    TECU: the slant is an L1-referenced delay in metres, so the comparison is
 *    @f$|\mathrm{slant}|/\texttt{tecu\_to\_L1m}@f$ with no mapping function.  A
 *    physical bound is of order 300 TECU.
 *  - **INC-3 `network.iono_dw_release_validate_k`** (double, 0 = off): the
 *    row-level screen for anything the reseed cannot reach (for example a
 *    code-bias-side park).  A LOS whose standardized deviation
 *    @f$|d_s| / \sigma_{rep}@f$ exceeds @f$k@f$ is held (its tie row is not folded)
 *    and re-tested every epoch until it passes.  The held set is rebuilt from
 *    scratch for every station this seam screens, so a LOS that sets or is pruned
 *    cannot leave a stale hold behind.
 *  - **INC-3b `network.iono_dw_admit_validate_k`** (double, 0 = off): the same
 *    screen applied at a LOS's first tie row, the edge described above.  A LOS the
 *    seam has not admitted is held for one epoch (during which `networkShDevVtec`
 *    still harvests its deviation, because that harvest is gated on the deweight
 *    weight and the local column, not on whether the row folded) and is admitted at
 *    the end of that epoch if the harvested deviation passes @f$k@f$.  A LOS for
 *    which no deviation arrives within a bounded wait is admitted unscreened and
 *    counted apart (`admit_nodev`).  On the first armed pass after start or resume,
 *    every present LOS is admitted unscreened (`admit_seeded`).  Quarantine and
 *    rejection are counted apart: every new LOS serves the one-epoch quarantine,
 *    and only a rejection is the guarded event.
 *
 * ## The screen statistic is the solved deviation, not the state
 *
 * The producer is a per-epoch absolute solve whose per-station StateManager is a
 * frozen linearization point, and every innovation is formed against it (data rows
 * through @c em.innovations, the tie row through @c r.innov = -x(iono_idx)).
 * Shifting @f$x_{lin}@f$ on local column @f$c@f$ by @f$\delta@f$ therefore moves
 * @f$y@f$ by exactly @f$-A_r e_c \delta@f$, and
 * @f[ b_{red} = u_s - N_{sr}N_{rr}^{-1}u_r \;\longrightarrow\;
 *     u_s - N_{sr}e_c\delta - N_{sr}N_{rr}^{-1}(u_r - N_{rr}e_c\delta) = b_{red} @f]
 * The reduced right-hand side is algebraically unchanged, and so is the absolute
 * estimate @f$x_{lin}+x_r@f$.  Consequently @f$|x_{lin} - \mathrm{field}|@f$ is a
 * frame-arbitrary number and is not screened on, and a bare state reseed is a solve
 * no-op.  The physical quantity is the deviation of the absolute slant from the
 * field, which `networkShDevVtec` has already harvested into
 * `ctx.carry.sh_dev_carry` earlier in the same epoch as
 * @f$d_s = (x_{lin} + x_r - \mathrm{SH})/M_0/\texttt{tecu\_to\_L1m}@f$ in vertical
 * TECU.  Every screen and bound here reads that, standardized by
 * @c representation_sigma_tecu in the same frame: dimensionless,
 * elevation-stationary, and free (no `reconstructStation`).
 *
 * The actuators in this card that are not frame-invariant are therefore the
 * **stash erase** (the entry sets the tie row's @c dev_pred_slant and
 * @c var_pred_slant, neither compensated by the linearization algebra), the
 * **ambiguity refloat** (under `carry_invariant` it drops the arc's DD chord from
 * @c A_old), and the **row hold** (a row-set change).  The slant state write is
 * kept as bookkeeping: it keeps the frame-dependent readouts meaningful and gives a
 * later structural change a sane starting point.  It is counted separately so it
 * is not mistaken for an effect.
 *
 * ## Ordering contract
 *
 * `networkIonoDwRelease` runs
 *   * **after `networkShHealth`**: that seam is the only caller of the gate's
 *     `observe()` / `advance()`, and the release edge is created by the slew inside
 *     `advance()`.  It also runs after `networkShDevVtec`, whose harvest is the
 *     epoch's deviation measurement update (erasing before it would discard valid
 *     information); both already precede this call site.
 *   * **in the same epoch as that `advance()`**, because the raised weight is read
 *     by the next epoch's PASS-B assembly: epoch N's assembly multiplies the tie
 *     rows by the weight left by `advance()` at the end of epoch N−1.  A response
 *     applied after the end of epoch N lets the parked value fold once.
 *   * guarded by @ref NetworkIonoDeweightGate::advanceCount: a datum-change epoch
 *     skips the gate block entirely (there is no `advance(false)` caller), so both
 *     of the gate's per-call lists retain the previous epoch's contents and an
 *     unguarded consumer would re-fire an old release.
 *
 * The ambiguity refloat cannot be applied here.  The slip funnel
 * `carry_slipped_ambs` is a per-epoch driver local whose last load-bearing consumer
 * (`buildCarryDdPriors`) has already run, and a bare `markAmbiguityPendingReseed`
 * issued at this point is cleared a few lines later by the leveling harvest's
 * `clearPendingReseed`.  The refloat is therefore queued into
 * @c ctx.carry.dw_release_pending and drained at the head of the next epoch, the
 * same pattern as `carry_innov_pending` (Card-4 D5), which is likewise a post-solve
 * detection feeding the next epoch's funnel.
 *
 * The seam owns no state: the deferred refloat queue, the held-row set and the gate
 * advance watermark live in @ref NetworkCarryState, and every counter in
 * @ref NetworkServiceCounters.  With `ctx.knobs.dw_hygiene_on` false it returns
 * immediately: the off path allocates nothing, writes no state and removes no row.
 */
#pragma once

// Kept light: app/NetworkRunContext.hpp includes this header for the tallies POD
// alone, so every heavy type in the signature is forward-declared and the real
// includes live in the .cpp (as in NetworkAnchorHygiene).
#include "common/PppObservation.hpp"   // std::vector<PppObservation> in the signature

#include <Eigen/Core>

#include <memory>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkEstimator;         // filter/NetworkEstimator.hpp
class NetworkIonoDeweightGate;  // filter/NetworkIonoDeweightGate.hpp
struct NetworkSetupResult;      // app/NetworkSetupResult.hpp
struct SyncedEpoch;             // rinex/MultiRinexSync.hpp (namespace-level)
struct NetworkRunContext;   // app/NetworkRunContext.hpp (included in the .cpp only)
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp (included in the .cpp only)

/**
 * @brief Run tallies for the card (standing invariant #4: run-end counters are the
 *        on-path telemetry).  Embedded in @c NetworkServiceCounters as `dwr`.
 *
 * @c epochs is the denominator: an armed run reporting 0 seam passes never ran the
 * response, which differs from a run in which the seam ran and no release occurred;
 * the log alone cannot tell the two apart.  @c releases_seen is the same test one
 * level down: the seam ran but the gate produced no edge, so every action count is
 * legitimately zero.
 */
struct NetworkIonoDwReleaseTallies {
    long long epochs = 0;             ///< seam passes run (denominator).
    long long gate_advances = 0;      ///< epochs on which the gate advanced (the opportunity for a release edge).
    long long releases_seen = 0;      ///< station-releases observed (weight rose above eps).
    long long reseed_stations = 0;    ///< stations whose slants were reseeded (INC-1).
    long long reseed_slants = 0;      ///< per-LOS slant-iono states written from the field (INC-1).
    long long reseed_refloats = 0;    ///< ambiguities queued for the next epoch's funnel (INC-1).
    long long reseed_stash_erased = 0;///< deviation-stash entries erased, counted apart from the v3k slip erase (INC-1).
    long long refloats_drained = 0;   ///< queued refloats delivered into the funnel at the next epoch's head (INC-1).
    long long park_reset_decoupled = 0;///< bound resets while the station's weight was <= eps (INC-2).
    long long park_reset_cold = 0;    ///< bound resets while the gate had no history for the station, i.e. post-warm-resume coverage (INC-2).
    double    park_max_tecu = 0.0;    ///< largest |slant| seen at a reset, in slant TECU (0 when none); shows how far past the bound the resets were (INC-2).
    long long rows_held = 0;          ///< LOS-epochs a tie row was withheld (INC-3).
    long long rows_held_entered = 0;  ///< distinct holds entered, i.e. the number of holds, which the LOS-epoch count cannot express (INC-3).
    long long rows_released = 0;      ///< holds released because the LOS passed k (INC-3).
    long long admit_held = 0;         ///< LOS-epochs a tie row was held pending admission, quarantine plus rejects (INC-3b).
    long long admit_admitted = 0;     ///< LOS admitted; their tie row folds from the next epoch (INC-3b).
    long long admit_rejected = 0;     ///< held LOS that had a harvested deviation and failed k: the guarded event, as distinct from the one-epoch quarantine every new LOS serves (INC-3b).
    long long admit_seeded = 0;       ///< LOS admitted unscreened by the one-shot start/resume seed (INC-3b).  Not an endorsement: a large value on a short leg means most of the run was never screened.
    long long admit_nodev = 0;        ///< LOS admitted because no deviation was harvested for them within the bounded barren wait (decoupled station or no local column); the guard fails open on absent evidence instead of withholding the row indefinitely (INC-3b).  A large value means the screen judged far fewer LOS than `admit_held` suggests.
};

/**
 * @brief Respond to this epoch's iono-deweight release edges, bound what a
 *        decoupled station may park, and re-screen the held tie rows.
 *        See the file banner for the ordering contract.
 *
 * @param ctx            [in/out] deferred refloat queue, held-row set, gate
 *                       watermark, counters, knobs.
 * @param config_        read for the SH / iono-weighting geometry only.
 * @param log            producer logger (debug/warn).
 * @param traces         IONO_DW `trace_events` rows.
 * @param setup          [in/out] station roster; a reseed writes station state.
 * @param se             this epoch (event stamps + sun-fixed longitude).
 * @param est            column lookups for the solved SH coefficients.
 * @param x_s            this epoch's solved shared state (the field being reseeded from).
 * @param est_index_of   per-station estimator index; < 0 for stations not solved.
 * @param modeled        this epoch's per-station modelled observations (LOS geometry).
 * @param iono_deweight  the gate, read-only (weights, release edges, watermark).
 */
void networkIonoDwRelease(NetworkRunContext& ctx,
                          const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces,
                          NetworkSetupResult& setup,
                          const SyncedEpoch& se,
                          const NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<int>& est_index_of,
                          const std::vector<std::vector<PppObservation>>& modeled,
                          const NetworkIonoDeweightGate& iono_deweight);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the ionosphere-deweight release hygiene.
 *
 * The ionosphere-deweight release hygiene (INC-1, INC-2, INC-3, INC-3b): seam passes,
 * gate advances, releases, reseeds, park-bound resets and the admission screen.
 *
 * Gate: returns 0 at once unless `ctx.knobs.dw_hygiene_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkDwReleaseHygieneRunEnd(const NetworkRunEndView& v);

/**
 * @brief Run-end report: the station-adaptive SH-VTEC constraint deweight.
 *
 * The station-adaptive SH-VTEC constraint deweight: the stations deweighted with the
 * adaptive engage and release events, or the static v1 list.
 *
 * Gate: returns 0 at once unless `idw_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkIonoDeweightRunEnd(const NetworkRunEndView& v);

/**
 * @brief Run-end report: the producer's state-manager re-inflations.
 *
 * The producer's state-manager re-inflations (card OWD, O2), summed over the stations,
 * on every run; the deweight-release hygiene's ionosphere re-inflation is the only
 * producer caller.
 *
 * Gate: prints always (a block).
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkReinflateRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
