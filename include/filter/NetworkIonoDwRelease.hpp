/**
 * @file NetworkIonoDwRelease.hpp
 * @brief IONO-DEWEIGHT **RELEASE** HYGIENE — reseed the slant states a zero-weight
 *        era left unvalidated, bound what such an era can park, and screen the
 *        tie rows the release re-engages.
 *        Card: docs/SPEC_deweight_release_hygiene.md.
 *
 * ## The defect
 *
 * The station-adaptive SH-VTEC-constraint deweight gate
 * (@ref GnssProcessor::NetworkIonoDeweightGate) drives a hostile station's weight
 * @f$w_s@f$ to zero, at which point the assembly appends NO SH↔slant tie rows for
 * it (ApplicationNetworkAssembly.cpp, @c networkAppendShIonoRows_ early-returns on
 * @f$w_s \le 0@f$).  That is the intended surgical decouple — but it also means
 * the station's per-LOS slant-iono states spend the whole era UNCONSTRAINED BY THE
 * FIELD, precisely while the storm that triggered the gate makes them
 * untrustworthy.  The network producer is a per-epoch ABSOLUTE solve whose
 * per-station StateManager holds a **frozen linearization point** — `setStateVector`
 * is called once per station, at setup (ApplicationNetworkSetup.cpp) — so a bad
 * slant/ambiguity pair does not decay: it PARKS, and re-presents a bit-identical
 * row every epoch.  Nothing in the local block objects, because the station's own
 * nuisances absorb it exactly.
 *
 * Then the gate RELEASES and the tie rows come back — silently, against those
 * parked states.
 *
 * ## ⚠ The valve, corrected against the control run (2026-08-01)
 *
 * The card was SPEC'd naming the deweight RELEASE as the valve at DOY-129 TOW
 * 329160.  The OFF control leg's own traces falsify that, and the correction is
 * recorded here because it decides which increment can ever fire:
 *
 *  - `runs/ft63_leg19_off/logs/network_events.csv` — **BSHM00ISR's only `IONO_DW`
 *    event in the entire leg is an ENGAGE at 329160** (z = 48.75, w slewing
 *    1.0 → 0.75).  It sat at @f$w = 1@f$, fully coupled, through the whole
 *    pre-break window; its z at the break is the gate REACTING to the break.
 *  - The `n_dw_zero = 3 / dw_min_w = 0.0` figures the SPEC attributes to BSHM are
 *    network AGGREGATES from @ref NetworkIonoDeweightGate::aggregate.  The stations
 *    actually at zero were LCK400IND / LMMF00MTQ / OAFA00ARG / POVE00BRA /
 *    SAVO00BRA, and those are the five that released at 329160.
 *  - The `ep_dw_*` harvest is POST-`advance()`, so the epoch-status row stamped
 *    329160 describes the weight field epoch 329190 will use.  The field that fed
 *    the 329160 solve is the row stamped 329130 — aggregate-identical to the four
 *    epochs before it.  **The release wave lands one epoch AFTER the break.**
 *  - `runs/ft63_leg19_rhsat/logs/rhs_attrib/rows_*.csv` — BSHM does gain a fifth
 *    `iono_sh_row` across the break, exactly as the SPEC says.  But with
 *    @f$w_s = 1@f$ on both sides, that row appeared because a satellite crossed the
 *    `elevation >= iw.elevation_cutoff_deg` / `h2local` admission gate and entered
 *    the tie stack for the FIRST time.
 *
 * The defect CLASS is diagnosed correctly — a slant column carrying an unvalidated
 * parked value is tied to the SH field and discharges into @f$a_{00}@f$.  The valve
 * is not the deweight gate; it is the admission gate.  INC-1 and INC-2 are both
 * keyed on the gate weight and therefore had ZERO OPPORTUNITY at that event (they
 * remain the correct guard for a genuine release, which the same day produced 22
 * engage / 15 release events).  **INC-3b is the increment that would have fired**,
 * and it is why this seam screens admission and not only release.
 *
 * Measured (DOY-129 2024-05-08, FT63 24 h all-on, replay from snap_20; named by the
 * reduced-RHS class-attribution instrument @c network.rhs_attrib_tow at closure
 * 5e-11): (BSHM00ISR, G07) — a C1C+L1C-only LOS, raw RINEX clean, LLI clean,
 * tracking continuous — carried a STEADY @f$+1.88\cdot10^{5}@f$ m prefit CODE
 * innovation (G16@BSHM @f$+7.1\cdot10^{4}@f$) against a garbage-parked slant-iono /
 * ambiguity pair; the phase side self-cancelled through the float ambiguity
 * (@f$y_{phase}=-6.4@f$ km) and the code row stayed naked at @f$w\approx2@f$.
 * BSHM's gate sat at weight 0 from 19:17 to 19:25:30 (n_dw_zero = 3), so the
 * garbage was absorbed locally — station @f$\hat\sigma_0@f$ clean, reduced C00 load
 * @f$+0.002@f$.  At TOW 329160 the gate released (n_dw_zero 3→0, dw_min 0.0→0.25,
 * the rate_limit slew; a fifth tie row appeared in BSHM's stack), the re-folded tie
 * coupled the slant column to the SH block through the Schur reduction, and the
 * code row's 188 km load leaked into @f$a_{00}@f$: BSHM's obs_code C00 contribution
 * @f$0.002 \to +6493@f$ (layer-2 @f$+3623@f$), the obs_code class balance flipped
 * @f$-5716 \to +1395@f$, the field discharged @f$+5.7@f$k TECU and the SH health
 * gate engaged for 76 min, leaving a 181 m between-satellite total_IF scar that
 * never healed.  **The row itself was IDENTICAL across the break — same @f$w@f$,
 * same @f$y@f$.  Only the COUPLING changed.**  That is why all seven prior
 * falsification arms (eq34 hygiene, dwfast, 4-sat ablation, signal integrity,
 * no-slip, adaptive iono q, code/satellite scans) left the break invariant: none of
 * them touched the release re-coupling or the parked cells.
 *
 * Standing invariant #1 in the project brief is exactly this class — "any zero-q /
 * stiff time-linked quantity MUST have a wired, tested slip/jump/event response".
 * A zero-weight era is a stiffness in the same sense (the state is time-linked to
 * nothing and therefore held), and RELEASE is its event.  This seam is that
 * response.
 *
 * ## What this seam does (three independently-knobbed increments)
 *
 *  - **INC-1 `network.iono_dw_release_reseed`** (bool): when a station's weight
 *    RISES above `network.iono_dw_release_floor_eps`, three things happen for that
 *    station, so the re-engaged ties fold INFORMATION rather than garbage:
 *      * every per-LOS slant-iono state is **reseeded from the current SH field
 *        evaluation** (a state WRITE — @c st.sm->setStateVector — plus a variance
 *        re-inflation).  Note this is NOT what erasing the deviation stash does:
 *        the stash only sets the row's PREDICTED deviation to zero, a deliberately
 *        weak assertion the per-constellation common-mode block (σ 50 m off
 *        diagonal) is designed NOT to enforce on the level — the parked metres
 *        would survive it untouched.
 *      * every float ambiguity of the station is **refloated through the standard
 *        slip funnel** — deferred one epoch (see the ordering contract) into BOTH
 *        `carry_slipped_ambs` and `carry_slipped_raw`, and marked
 *        @c markAmbiguityPendingReseed.  The raw set is load-bearing: the Iono-2
 *        v3k `d5_keep` policy KEEPS a deviation level for a non-raw re-tap, which
 *        would resurrect exactly the parked value this card exists to discard.
 *        The ambiguity re-seed reads the CURRENT iono state
 *        (@c StateManager::computeAmbiguitySeed takes @c x_now), so the slant
 *        reseed above must land first — it does, one epoch earlier by construction.
 *      * the station's per-LOS deviation stash entries are **erased** outright.
 *  - **INC-2 `network.iono_dw_park_bound_tecu`** (double, 0 = off): a slant-iono
 *    state whose magnitude exceeds the bound is reset to the field evaluation
 *    immediately, without waiting for a release.  Applied while the station is
 *    DECOUPLED (weight ≤ eps) and, additionally, while the gate has NO history for
 *    it (`samples() == 0`) — the post-warm-resume condition, since no gate state is
 *    serialized and a resumed leg starts every station at @f$w=1@f$ and re-folds
 *    every tie row on its FIRST assembly with no release edge to hook.  The two
 *    triggers are counted separately.  The bound is in **slant TECU**: the state is
 *    an L1-referenced slant delay in metres, so the comparison is
 *    @f$|x|/\texttt{tecu\_to\_L1m}@f$ with no mapping function — a physical
 *    O(300 TECU) bound sits three orders below the observed 1.15e6-TECU park.
 *  - **INC-3 `network.iono_dw_release_validate_k`** (double, 0 = off): the
 *    row-level screen for anything the reseed cannot reach (e.g. a code-bias-side
 *    park).  A LOS whose standardized deviation @f$|d_s| / \sigma_{rep}@f$ exceeds
 *    @f$k@f$ is HELD — its tie row is not folded — and re-tested every epoch until
 *    it passes.  The held set is rebuilt from scratch for every station this seam
 *    sweeps, so a LOS that sets or is pruned cannot leave a stale hold behind.
 *  - **INC-3b `network.iono_dw_admit_validate_k`** (double, 0 = off): the SAME
 *    screen applied at a LOS's FIRST tie row.  See "the valve" below — this is the
 *    increment with actual opportunity at the measured event.  A LOS the seam has
 *    not admitted is held for one epoch (during which `networkShDevVtec` still
 *    harvests its deviation — that harvest is gated on the deweight weight, not on
 *    whether the row folded) and admitted at the end of that epoch iff the
 *    harvested deviation passes @f$k@f$.  Quarantine and rejection are counted
 *    apart: every new LOS serves the one-epoch quarantine, but only a rejection is
 *    the guarded event.
 *
 * ## The screen statistic is the SOLVED deviation, never the state
 *
 * The producer is a per-epoch ABSOLUTE solve whose per-station StateManager is a
 * frozen LINEARIZATION POINT, and every innovation is formed against it (data rows
 * through @c em.innovations, the tie row through @c r.innov = -x(iono_idx)).
 * Shifting @f$x_{lin}@f$ on local column @f$c@f$ by @f$\delta@f$ therefore moves
 * @f$y@f$ by exactly @f$-A_r e_c \delta@f$, and
 * @f[ b_{red} = u_s - N_{sr}N_{rr}^{-1}u_r \;\longrightarrow\;
 *     u_s - N_{sr}e_c\delta - N_{sr}N_{rr}^{-1}(u_r - N_{rr}e_c\delta) = b_{red} @f]
 * — algebraically unchanged, and the absolute estimate @f$x_{lin}+x_r@f$ with it.
 * **So @f$|x_{lin} - \mathrm{field}|@f$ is a frame-arbitrary number and must never be
 * screened on, and a bare state reseed is a solve NO-OP.**  The physical quantity is
 * the deviation of the ABSOLUTE slant from the field, which `networkShDevVtec` has
 * already harvested into `ctx.carry.sh_dev_carry` earlier in the same epoch as
 * @f$d_s = (x_{lin} + x_r - \mathrm{SH})/M_0/\texttt{tecu\_to\_L1m}@f$ in vertical
 * TECU.  Every screen and bound here reads that, standardized by
 * @c representation_sigma_tecu in the same frame — dimensionless,
 * elevation-stationary, and free (no `reconstructStation`).
 *
 * The three genuinely non-invariant actuators in this card are therefore: the
 * **stash erase** (the entry sets the tie row's @c dev_pred_slant and
 * @c var_pred_slant, neither compensated by the linearization algebra), the
 * **ambiguity refloat** (under `carry_invariant` it drops the arc's DD chord from
 * @c A_old), and the **row hold** (a row-set change).  The slant state write is
 * retained as BOOKKEEPING — it makes the frame-broken readouts honest and gives a
 * later structural change a sane starting point — and is counted separately so it
 * can never be mistaken for an effect.
 *
 * ## Ordering contract
 *
 * `networkIonoDwRelease` MUST run
 *   * **AFTER `networkShHealth`** — that seam is the ONLY caller of the gate's
 *     `observe()` / `advance()`, and the release edge is created by the slew inside
 *     `advance()`.  It must also be after `networkShDevVtec`, whose harvest is the
 *     epoch's legitimate deviation measurement update (erasing before it would
 *     discard good information), and both already precede this call site.
 *   * **in the SAME epoch as that `advance()`**, because the weight it raised is
 *     read by the NEXT epoch's PASS-B assembly: epoch N's assembly multiplies the
 *     tie rows by the weight left by `advance()` at the end of epoch N−1.  A
 *     response applied any later than the end of epoch N has already let the
 *     garbage fold once.
 *   * guarded by @ref NetworkIonoDeweightGate::advanceCount — a datum-change epoch
 *     skips the gate block entirely (there is no `advance(false)` caller), so both
 *     of the gate's per-call lists retain the previous epoch's contents and an
 *     unguarded consumer would re-fire an old release.
 *
 * The one action that CANNOT be applied here is the ambiguity refloat: the slip
 * funnel `carry_slipped_ambs` is a per-epoch driver local whose last load-bearing
 * consumer (`buildCarryDdPriors`) already ran, and a bare
 * `markAmbiguityPendingReseed` issued at this point is wiped a few lines later by
 * the leveling harvest's `clearPendingReseed`.  So the refloat is queued into
 * @c ctx.carry.dw_release_pending and drained at the HEAD of the next epoch — the
 * established `carry_innov_pending` (Card-4 D5) pattern, which is likewise a
 * post-solve detection feeding the next epoch's funnel.
 *
 * It owns NO state: the deferred refloat queue, the held-row set and the gate
 * advance watermark all live in @ref NetworkCarryState, and every counter in
 * @ref NetworkServiceCounters.  With `ctx.knobs.dw_hygiene_on` false it returns
 * immediately, having touched nothing — the OFF path allocates nothing, writes no
 * state, and removes no row.
 */
#pragma once

// Deliberately LIGHT: this header is pulled in by app/NetworkRunContext.hpp for the
// tallies POD alone, so every heavy type in the signature is forward-declared and the
// real includes live in the .cpp (the NetworkAnchorHygiene precedent).
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
struct SyncedEpoch;             // rinex/MultiRinexSync.hpp — namespace-level
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

/**
 * @brief Run tallies for the card (standing invariant #4 — counters or it didn't
 *        happen).  Embedded in @c NetworkServiceCounters as `dwr`.
 *
 * @c epochs is the DENOMINATOR: an armed run reporting 0 seam passes never ran the
 * response at all, which is NOT the same as "it ran and there was no release to
 * respond to" — and no amount of clean-looking log output would tell the two apart.
 * @c releases_seen is the same test one level down (the seam ran, but the gate
 * never produced an edge, so every action count is legitimately zero).
 */
struct NetworkIonoDwReleaseTallies {
    long long epochs = 0;             ///< seam passes run (denominator).
    long long gate_advances = 0;      ///< epochs on which the gate actually advanced (the release-edge OPPORTUNITY).
    long long releases_seen = 0;      ///< station-releases observed (weight rose above eps).
    long long reseed_stations = 0;    ///< INC-1: stations whose slants were reseeded.
    long long reseed_slants = 0;      ///< INC-1: per-LOS slant-iono states written from the field.
    long long reseed_refloats = 0;    ///< INC-1: ambiguities queued for the next epoch's funnel.
    long long reseed_stash_erased = 0;///< INC-1: deviation-stash entries erased (counted apart from the v3k slip erase).
    long long refloats_drained = 0;   ///< INC-1: queued refloats actually delivered into the funnel at the next epoch's head.
    long long park_reset_decoupled = 0;///< INC-2: bound resets while the station's weight was <= eps (the spec's own trigger).
    long long park_reset_cold = 0;    ///< INC-2: bound resets while the gate had NO history for the station (post-warm-resume coverage).
    double    park_max_tecu = 0.0;    ///< INC-2: largest |slant| seen at a reset, in slant TECU (0 when none — the "how far past the bound" scale).
    long long rows_held = 0;          ///< INC-3: LOS-epochs a tie row was withheld.
    long long rows_held_entered = 0;  ///< INC-3: distinct holds ENTERED (a hold's count, which its duration cannot express).
    long long rows_released = 0;      ///< INC-3: holds released because the LOS finally passed k.
    long long admit_held = 0;         ///< INC-3b: LOS-epochs a tie row was held pending admission (quarantine + rejects).
    long long admit_admitted = 0;     ///< INC-3b: LOS admitted (their tie row folds from the next epoch).
    long long admit_rejected = 0;     ///< INC-3b: of those held, the ones that HAD a harvested deviation and FAILED k — the guarded event, as distinct from the one-epoch quarantine every new LOS serves.
    long long admit_seeded = 0;       ///< INC-3b: LOS admitted UNSCREENED by the one-shot start/resume seed.  Never an endorsement — a large number here on a short leg means most of the run was never screened at all.
    long long admit_nodev = 0;        ///< INC-3b: LOS admitted because no deviation was EVER harvested for them (decoupled station / no local column) — the guard failing open on absent evidence rather than starving the row.  A large number means the screen is judging far fewer LOS than `admit_held` suggests.
};

/**
 * @brief Respond to this epoch's iono-deweight RELEASE edges, bound what a
 *        decoupled station may park, and re-screen the held tie rows.
 *        See the file banner for the ordering contract.
 *
 * @param ctx            [in/out] deferred refloat queue, held-row set, gate
 *                       watermark, counters, knobs.
 * @param config_        read for the SH / iono-weighting geometry only.
 * @param log            producer logger (debug/warn).
 * @param traces         IONO_DW `trace_events` rows.
 * @param setup          [in/out] station roster — a reseed WRITES station state.
 * @param se             this epoch (event stamps + sun-fixed longitude).
 * @param est            column lookups for the solved SH coefficients.
 * @param x_s            this epoch's solved shared state (the field being reseeded from).
 * @param est_index_of   per-station estimator index; < 0 for stations not solved.
 * @param modeled        this epoch's per-station modelled observations (LOS geometry).
 * @param iono_deweight  the gate — READ ONLY (weights, release edges, watermark).
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

} // namespace GnssProcessor
