#pragma once
/**
 * @file NetworkPhaseDatum.hpp
 * @brief Phase-bias↔ambiguity datum maintenance for the PPP-RTK network
 *        producer: the prioritized-datum spectrum-based drop-and-heal.
 *
 * The prioritized union registers a phase-bias column for every (sat,signal)
 * that any two stations track.  Two things can leave such a column below the
 * rank-gate cutoff and would withhold the WHOLE epoch (every well-anchored
 * satellite lost to one bad column): (1) its ONLY ambiguity pin lived in an
 * anchor station that was DROPPED in the serial consume (a rank-deficient
 * local block) — a pure near-null orphan; (2) the joint W6 code-bias
 * estimation leaves a marginally-tracked satellite's phase bias weakly
 * separated from its clock — a near-null-but-real column.  The heal
 * soft-anchors exactly those columns toward 0 (σ = 1 m: far above the cutoff
 * so the residual gauge is resolved, negligible against the 1e10 ambiguity
 * pins so well-anchored columns are untouched) — a bounded, signalled
 * one-epoch degradation (the emitter bias-jump disc bumps when the proper
 * ambiguity anchor returns).
 *
 * Seeded at decomposition INC-8 (2026-07-17), which relocated
 * healNearNullPhaseBias VERBATIM out of ApplicationNetwork.cpp; the candidate
 * census + ensemble datum + anchor election joined at INC-8b (2026-07-22),
 * which relocated them VERBATIM from the same place (the Card-3 eq34 chain
 * election went to the sibling @ref NetworkChainDatum).  See
 * docs/SPEC_inc8b_election_split.md.  These functions own NO state: every
 * carry/workspace object is an explicit reference into the caller's
 * NetworkRunContext / EpochWorkspace.
 *
 * The three INC-8b entry points realize the phase-bias gauge three different
 * ways; the run TU's `if/else-if/else` mode dispatch stays the contract, and
 * exactly one election runs per epoch over a census built once:
 *
 *   networkBuildPhaseDatumCandidates   (always, under phase_datum_prioritized)
 *     ├── networkElectEnsembleDatum    network.phase_datum_ensemble
 *     ├── networkElectChainDatum       network.phase_datum_eq34  [NetworkChainDatum]
 *     └── networkElectPhaseAnchors     otherwise (the anchor election)
 */

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"    // NetStation / PhaseDatumCandidates
#include "filter/NetworkAnchorHygiene.hpp"  // AnchorHygiene / NetworkAnchorTallies

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    struct NetworkInjectState;   // filter/NetworkInject.hpp — the EVENT HARNESS directive set


class GnssConfig;
struct PppObservation;

/// One soft-anchored phase-bias column (Card PA2 attribution): which
/// (sat, signal) was healed, the eigenvalue of the near-null direction it spans
/// most heavily, and that |component|.  The caller emits these as @c trace_events
/// rows so a healed column is attributable from the trace alone — a run that
/// heals every epoch otherwise "succeeds" silently (the heal-success log is debug).
struct PhaseHealEvent {
    std::string sat;      ///< satellite id
    std::string signal;   ///< signal id
    double      ev{0.0};  ///< eigenvalue of the most-spanned near-null direction
    double      span{0.0};///< |component| of this column in that direction (> kSpanFrac)
};

/**
 * @brief Identify near-null phase-bias directions of the reduced shared
 *        normals and soft-anchor exactly those columns toward 0 (σ = 1 m), so
 *        one weak column cannot withhold the whole epoch.
 *
 * The cutoff is @ref NetworkEstimator::rankGateTol · max(λmax, 1) —
 * deliberately the SAME resolved cutoff as the rank gate this heal protects
 * (Card DG2; it defaults to @ref NetworkEstimator::kNullSpaceTol).  Reading the
 * gate's own value is load-bearing: with an ARMED (deeper) tolerance the heal
 * must NOT keep soft-anchoring the weak-but-real families the calibration just
 * moved above the cutoff — that would be prior information on real parameters.
 * A direction counts as a heal target when a phase-bias column spans it with
 * |component| > 0.3 (a clock-gauge direction has ~zero phase-bias weight, so
 * its minor PB participants stay below the fraction and are left alone).
 *
 * ★ PERF: this does a FULL eigenVECTOR decomposition of the reduced normals
 * (O(n_shared³) — ~1156³ at 305 stations), so invoke it ONLY from the
 * gate-failure path, never every epoch.  A healthy network passes the cheap
 * eigenvalue-only gate on the first try and never pays this cost.
 *
 * @param est                     Joint estimator (non-const:
 *                                reducedSharedNormals lazily reduces;
 *                                addSharedPrior folds the heal anchors).
 * @param shared_phase_keys       This epoch's shared phase-bias key set.
 * @param phase_datum_prioritized network.phase_datum_prioritized; false ⇒
 *                                immediate 0 (the legacy datum heals nothing).
 * @param within_epoch_armed      network.raim_within_epoch: mirror the heal
 *                                anchors into @p cap_priors for the replay.
 * @param cap_priors              RAIM-replay capture channel (ws.cap_priors —
 *                                see the EpochWorkspace writer-site contract in
 *                                filter/NetworkRunTypes.hpp).
 * @param healed_out              [out, optional] APPENDED per-column attribution
 *                                (@ref PhaseHealEvent) for the caller's
 *                                trace_events rows; @c nullptr (the default) is
 *                                the pre-PA2 behaviour exactly.  Purely
 *                                observational — it never changes which columns
 *                                are selected or what prior is folded.
 * @return Number of phase-bias columns soft-anchored (0 = nothing healed /
 *         not prioritized / eigensolve failed).
 */
int healNearNullPhaseBias(NetworkEstimator& est,
                          const std::set<std::string>& shared_phase_keys,
                          bool phase_datum_prioritized,
                          bool within_epoch_armed,
                          std::vector<NetworkEstimator::SharedPrior>& cap_priors,
                          std::vector<PhaseHealEvent>* healed_out = nullptr);

/**
 * @brief Build this epoch's prioritized phase-datum candidate census and apply
 *        the network.phase_datum_min_stations coverage gate.
 *
 * Relocated VERBATIM from Application::runNetwork at INC-8b.  Runs for EVERY
 * prioritized realization (ensemble / eq34 chain / anchor election), exactly
 * once per epoch, before the mode dispatch picks an election.
 *
 * NOTE on the timing (preserved from the original site): the ambiguity states
 * are allocated later, in PASS B's computeMeasurementUpdate, so this cannot
 * test getAmbiguityIndex — it keys off the phase-bias states PASS A allocated
 * through the same usability gate as the row build.
 *
 * ⚠ SIDE EFFECT: when the coverage gate fires it PRUNES each tracking
 * station's satellite phase-bias states (StateManager::pruneSatPhaseBiasStatesExcept)
 * so per-station column sets stay consistent with the gated union.  BOTH
 * elections depend on that prune having already happened, and this function is
 * the sole writer of the two run-end gate counters — computing the census twice
 * would double-count them (standing invariant #4 telemetry).
 *
 * @param config_             networkPhaseDatumPrefElevDeg / MinStations.
 * @param stations            [in/out] non-const: the gate prunes PB states.
 * @param present             This epoch's presence mask.
 * @param modeled             Pruned per-station observations (elevation source).
 * @param cnd                 [out] census; every member is fully overwritten,
 *                            so caller storage may be reused.
 * @param n_gated_epochs      [in/out] ctx.counters run-end telemetry.
 * @param n_gated_key_epochs  [in/out] ctx.counters run-end telemetry.
 */
void networkBuildPhaseDatumCandidates(
        const GnssConfig& config_,
        std::vector<NetStation>& stations,
        const std::vector<bool>& present,
        const std::vector<std::vector<PppObservation>>& modeled,
        PhaseDatumCandidates& cnd,
        std::size_t& n_gated_epochs,
        std::size_t& n_gated_key_epochs);

/**
 * @brief Fix-A ANCHOR-FREE ENSEMBLE datum: the shared phase-bias set is the
 *        coverage-gated union, every station's pins are cleared and NO anchor
 *        is elected.  Relocated VERBATIM at INC-8b.
 *
 * The gauge is realized instead by the kernel sum-to-zero fold built from every
 * tracking station's ambiguity column AFTER the reduce (buildEnsembleGauges,
 * which stays in the run TU — it runs post-beginEpoch/PASS-B).  With no station
 * holding a pinned_amb_key, do_fold stays false and isGaugePinned false, so
 * every station carries all its arcs and there is no anchor to hand over.
 *
 * @param this_epoch_anchor  [out] cleared; the ensemble elects no anchor.  MUST
 *                           be the run's ctx.carry instance (the emit-site
 *                           handover loop is its sole consumer).
 */
void networkElectEnsembleDatum(
        std::vector<NetStation>& stations,
        const PhaseDatumCandidates& cnd,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys);

/**
 * @brief Prioritized per-(sat,signal) ANCHOR election with stickiness.
 *        Relocated VERBATIM at INC-8b.
 *
 * Keeps the sticky incumbent while it is present AND still holds the key
 * (fewest handovers), else takes the highest-ranked candidate of that
 * constellation that tracks it — concentrating most of a constellation on one
 * receiver (frame-consistent) with graceful per-satellite fallback.  The pin
 * lands in the anchor's OWN pinned_amb_targets at target 0.0 — its local block,
 * the load-bearing invariant.  A key with no tracker is dropped from
 * @p shared_phase_keys entirely, which is what keeps the shared set and the pin
 * set in bijection (that orphan case is exactly what @ref healNearNullPhaseBias
 * exists to rescue when it slips through).
 *
 * @param sticky            network.phase_datum_sticky (passed as a scalar so
 *                          this header needs no GnssConfig definition).
 * @param hyg               eq34 REATTACH HYGIENE verdicts (see
 *                          filter/NetworkAnchorHygiene.hpp).  Default-constructed
 *                          (inactive) this is the pre-card election exactly.  When
 *                          active it makes a converged candidate outrank a fresh one
 *                          on a REATTACH (the sticky incumbent is exempt — keeping it
 *                          is not a reattach), leaves a BRIDGED key in the shared set
 *                          with no pin at all (its soft shared row is the datum), and
 *                          supplies the INC-3 σ multiplier for an unavoidable fresh
 *                          pin.
 * @param tally             [in/out] run counters (reattaches / cold fallbacks /
 *                          ramped pins).  Pass @c ctx.counters.hyg.
 * @param anchor_incumbent  [in/out] sticky baseline; swapped at the end.  MUST
 *                          be the run's ctx.carry instance — a copy silently
 *                          degrades the election to a fresh one every epoch,
 *                          which is invisible except as handover churn.
 * @param this_epoch_anchor [out] read at the emit site for handover signalling.
 * @param handover          TWO-EDGE CONTINUOUS HANDOVER
 *                          (docs/SPEC_pivot_visibility_gauge_defect.md §5.1).
 *                          @c nullptr (the default) is the pre-card election
 *                          EXACTLY: every pin target is 0.0.  When supplied, a
 *                          handover pins the INCOMING anchor at its own absolute
 *                          ambiguity estimate from the ledger — i.e. at its value
 *                          in the OUTGOING frame — so the gauge change is an
 *                          S-transform and the emitted (sat,signal) family does
 *                          not step.  A successor with no ledger entry (fresh arc
 *                          / first epoch) still pins at 0 and is counted as a COLD
 *                          handover: an unavoidable re-realization the consumer is
 *                          TOLD about is a different object from a silent one.
 *                          R2 (§5.09): a continuous target is STICKY — it is
 *                          stored in @p anchor_pin_target and re-applied every
 *                          epoch the incoming anchor remains incumbent, so the
 *                          inherited frame persists for the anchor's whole
 *                          tenure instead of reverting to 0 at T+1 (the §5.07.3
 *                          one-epoch deferral that made the unsticky version
 *                          measure null).  A key with no stored target keeps its
 *                          0.0 pin, so a settled network that never handed over
 *                          is byte-identical even with the ledger supplied.
 * @param handovers_out     [out, optional] APPENDED per-handover attribution
 *                          (@ref AnchorHandoverEvent) for the caller's
 *                          @c trace_events rows — BOTH edges in one record.
 *                          Purely observational: it never changes which station is
 *                          elected or what target is pinned.  Populated whenever
 *                          it is non-null, including on the @c handover ==
 *                          @c nullptr path, so the churn a legacy arm suffers can
 *                          be MEASURED before the fix is armed.
 * @param reref_out         [out, optional] QUEUE item 1(d)
 *                          (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1):
 *                          INSERTED (sat, signal) of every COLD handover this
 *                          epoch — the families whose datum re-realized and whose
 *                          carried state therefore belongs to the RETIRED frame.
 *                          A CONTINUOUS handover is deliberately NOT marked
 *                          (§5.09): under the sticky target the successor is
 *                          pinned in the outgoing frame for its whole tenure, the
 *                          family provably does not step at T or T+1, and the
 *                          carried state remains in-frame — erasing it would be
 *                          exactly the arc-truncation this card exists to stop
 *                          (measured: 582/621 cold handovers erased 10 236
 *                          leveling carries on a 12 h zero-slip fixture).  The
 *                          caller routes it to ctx.carry.phase_anchor_reref, where
 *                          the D2/C1 shared-carry consume marginalizes those PB
 *                          keys for one epoch (exactly as @c eq34_reref) and
 *                          @ref networkApplyPhaseAnchorCarryReref erases the
 *                          family's phase-leveling local carries.  @c nullptr
 *                          (the default, and the knob-OFF path) is byte-identical:
 *                          nothing is populated and nothing downstream fires.
 *                          Unlike @p handovers_out this is NOT purely
 *                          observational — its consumers change which priors fold.
 * @param inject        EVENT HARNESS directive set (filter/NetworkInject.hpp), or
 *                      @c nullptr — which is the normal path AND every unarmed
 *                      run.  When a staged @c anchor_handover covers a family,
 *                      the incumbent is excluded from candidacy for THIS epoch
 *                      only (forcing a successor), and in @c cold mode the
 *                      continuous-handover branch is refused so the family takes
 *                      the honest re-realize + re-reference path.  The directive
 *                      records which station it retired and which was elected.
 */
void networkElectPhaseAnchors(
        bool sticky,
        std::vector<NetStation>& stations,
        const std::vector<bool>& present,
        const PhaseDatumCandidates& cnd,
        const AnchorHygiene& hyg,
        NetworkAnchorTallies& tally,
        std::map<std::string, std::string>& anchor_incumbent,
        std::map<std::string, double>& anchor_pin_target,
        std::map<std::string, std::string>& this_epoch_anchor,
        std::set<std::string>& shared_phase_keys,
        const AnchorHandoverLedger* handover = nullptr,
        std::vector<AnchorHandoverEvent>* handovers_out = nullptr,
        double max_var = 0.0,
        std::set<std::pair<std::string, std::string>>* reref_out = nullptr,
        double max_var_ratio = 0.0,
        const NetworkInjectState* inject = nullptr);

/**
 * @brief QUEUE item 1(d): erase the phase-leveling local ambiguity carries of
 *        every re-referenced (sat, signal) family, at EVERY station.
 *
 * The local ambiguities are frame-DEPENDENT quantities (z ≈ range − bˢ_σ − …),
 * so when the family's phase-bias datum re-realizes at a handover the carried
 * floats are stale in exactly the way the shared PB carry is; they refloat next
 * epoch.  Verbatim the eq34 D4 carry-hygiene erase
 * (@c NetworkChainDatum.cpp), applied to the anchored election's re-reference
 * set.  Runs at the election site, BEFORE PASS B consumes the carry.
 *
 * @param reref        This epoch's re-referenced (sat, signal) families
 *                     (ctx.carry.phase_anchor_reref).  Empty ⇒ immediate 0.
 * @param local_carry  ctx.carry.local_carry — the phase-leveling per-station
 *                     ambiguity carry, keyed (station idx, sat, signal).  Inert
 *                     (empty map) unless network.phase_leveling.
 * @return Number of local carry entries erased.
 */
int networkApplyPhaseAnchorCarryReref(
        const std::set<std::pair<std::string, std::string>>& reref,
        std::map<std::tuple<int, std::string, std::string>, LocalCarry>& local_carry);

} // namespace GnssProcessor
