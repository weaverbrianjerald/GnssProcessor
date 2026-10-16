#pragma once
/**
 * @file PhaseDatum.hpp
 * @brief Phase-bias datum seam of the PPP-RTK network producer: the prioritized
 *        candidate census and coverage gate, the anchor election
 *        with its handover, held S-basis, candidacy, gate-drop and re-entry options,
 *        the anchor-slip handover staging, the re-reference carry erase, and the
 *        near-null phase-bias soft-anchor.
 *
 * Near-null soft-anchor (@ref healNearNullPhaseBias).  The prioritized union registers
 * a phase-bias column for every (sat,signal) that any two stations track.  Two cases
 * can leave such a column below the rank-gate cutoff, and the gate would then
 * withhold the whole epoch, losing every well-anchored satellite to one column:
 * (1) the column's only ambiguity pin was at an anchor station dropped in the serial
 * consume (a rank-deficient local block), which leaves a pure near-null orphan;
 * (2) the joint W6 code-bias estimation leaves a marginally tracked satellite's phase
 * bias weakly separated from its clock, a near-null but real column.  Those columns
 * are soft-anchored toward 0 with σ = 1 m: far above the cutoff, so the residual
 * gauge is resolved, and negligible against the 1e10 ambiguity pins, so
 * well-anchored columns are unaffected.  The result is a bounded one-epoch
 * degradation that is signalled: the emitter's bias-jump discontinuity counter
 * increments when the proper ambiguity anchor returns.
 *
 * The functions were relocated unchanged out of ApplicationNetwork.cpp at
 * decomposition INC-8 (the soft-anchor) and INC-8b (the census and the anchor
 * election; the eq34 chain election went to the sibling
 * @ref NetworkChainDatum).  See docs/SPEC_inc8b_election_split.md.  They own no
 * state: every carry and workspace object is an explicit reference into the
 * caller's NetworkRunContext / EpochWorkspace.
 *
 * The two elections realize the phase-bias gauge in two different ways.  The run
 * TU's `if/else` mode dispatch is the contract: one election runs per epoch, over a
 * census built once:
 *
 *   networkBuildPhaseDatumCandidates   (always, under phase_datum_prioritized)
 *     ├── networkElectChainDatum       network.phase_datum_eq34  [NetworkChainDatum]
 *     └── networkElectPhaseAnchors     otherwise (the anchor election)
 */

#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"        // net::ArcTable: the arcs' leveling carry
#include "network/driver/RunTypes.hpp"    // NetStation / PhaseDatumCandidates
#include "network/solve/AnchorHygiene.hpp"  // AnchorHygiene / NetworkAnchorTallies
#include "network/solve/PhaseGateDrop.hpp"  // PhaseGateDropEvent and the edge rule
#include "network/solve/AnchorReentry.hpp"  // AnchorReentry and the drop memo

#include <cstddef>
#include <map>
#include <set>
#include <tuple>    // the local-carry key (station, sat, signal)
#include <string>
#include <vector>
#include <cstdint>

namespace GnssProcessor {

    struct NetworkInjectState;   // network/carry/Inject.hpp: the event-harness directive set


class GnssConfig;
struct PppObservation;

/// One soft-anchored phase-bias column: the (sat, signal), the eigenvalue of the
/// near-null direction the column spans most heavily, and that |component|.  The
/// caller emits these as @c trace_events rows so every soft-anchored column is
/// attributable from the trace; the success log line is at debug level, so without
/// the rows a run that soft-anchors every epoch shows nothing.
struct PhaseHealEvent {
    std::string sat;      ///< satellite id
    std::string signal;   ///< signal id
    double      ev{0.0};  ///< eigenvalue of the most-spanned near-null direction
    double      span{0.0};///< |component| of this column in that direction (> kSpanFrac)
};

/**
 * @brief Identify near-null phase-bias directions of the reduced shared
 *        normals and soft-anchor those columns toward 0 (σ = 1 m), so that
 *        one weak column does not withhold the whole epoch.
 *
 * The cutoff is @ref NetworkEstimator::rankGateTol · max(λmax, 1), the same
 * resolved cutoff as the rank gate this function protects (it defaults to
 * @ref NetworkEstimator::kNullSpaceTol).  Reading the gate's own value matters: with
 * an armed (deeper) tolerance, the families the calibration moved above the cutoff
 * are weak but real, and soft-anchoring them would put prior information on real
 * parameters.  A direction is a target when a phase-bias column spans it with
 * |component| > 0.3 (kSpanFrac).  A clock-gauge direction has almost no phase-bias
 * weight, so its minor PB participants stay below that fraction and are left alone.
 *
 * Cost: a full eigenvector decomposition of the reduced normals, O(n_shared³)
 * (n_shared is about 1156 at 305 stations).  Call it only from the gate-failure
 * path, not every epoch.  A healthy network passes the eigenvalue-only gate on the
 * first try and never reaches this function.
 *
 * @param est                     Joint estimator (non-const:
 *                                reducedSharedNormals reduces lazily;
 *                                addSharedPrior folds the soft anchors).
 * @param shared_phase_keys       This epoch's shared phase-bias key set.
 * @param phase_datum_prioritized network.phase_datum_prioritized; false returns
 *                                0 immediately (the legacy datum is not treated).
 * @param within_epoch_armed      network.raim_within_epoch: mirror the soft
 *                                anchors into @p cap_priors for the replay.
 * @param cap_priors              RAIM-replay capture channel (ws.cap_priors;
 *                                see the EpochWorkspace writer-site contract in
 *                                network/driver/RunTypes.hpp).
 * @param healed_out              [out, optional] per-column attribution
 *                                (@ref PhaseHealEvent), appended, for the caller's
 *                                trace_events rows.  @c nullptr (the default)
 *                                records nothing.  Observational only: it does not
 *                                change which columns are selected or what prior
 *                                is folded.
 * @param cap_prior_kinds         [out, optional] the capture's kinds
 *                                (ws.cap_prior_kinds): one @c PbHeal per soft anchor
 *                                appended to @p cap_priors, so the two stay in step
 *.  @c nullptr (the default) appends
 *                                none, and a replay of that capture is recorded under
 *                                one unnamed kind.
 * @return Number of phase-bias columns soft-anchored (0 when nothing is below the
 *         cutoff, the datum is not prioritized, or the eigensolve fails).
 */
int healNearNullPhaseBias(NetworkEstimator& est,
                          const std::set<std::string>& shared_phase_keys,
                          bool phase_datum_prioritized,
                          bool within_epoch_armed,
                          std::vector<NetworkEstimator::SharedPrior>& cap_priors,
                          std::vector<PhaseHealEvent>* healed_out = nullptr,
                          std::vector<ContributionKind>* cap_prior_kinds = nullptr);

/**
 * @brief Build this epoch's prioritized phase-datum candidate census and apply
 *        the network.phase_datum_min_stations coverage gate.
 *
 * Relocated unchanged from Application::runNetwork (INC-8b).  Runs for every
 * prioritized realization (eq34 chain, anchor election), once per epoch,
 * before the mode dispatch picks an election.
 *
 * Timing: the ambiguity states are allocated later, in PASS B's
 * computeMeasurementUpdate, so this function cannot test getAmbiguityIndex.  It
 * keys off the phase-bias states PASS A allocated through the same usability gate
 * as the row build.
 *
 * Side effect: when the coverage gate excludes keys, it prunes those keys'
 * phase-bias states at every present station
 * (StateManager::pruneSatPhaseBiasStatesExcept), so the per-station column sets stay
 * consistent with the gated union.  Both elections depend on that prune having
 * happened.  This function is also the sole writer of the two run-end gate
 * counters, so computing the census twice would double-count them (standing
 * invariant #4 telemetry).
 *
 * @param config_             networkPhaseDatumPrefElevDeg / MinStations.
 * @param stations            [in/out] non-const: the gate prunes PB states.
 * @param present             This epoch's presence mask.
 * @param modeled             Pruned per-station observations (elevation source).
 * @param cnd                 [out] census, including the gate's exclusions
 *                            (@c gated_keys); every member is overwritten, so
 *                            caller storage may be reused.
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

/// Held S-basis bundle for the anchor election, and the counters the election writes
/// back (RF-2).
///
/// Passed as a pointer that is null on every unarmed run, so with it off the election
/// is byte-identical: its existing branches are unchanged and nothing below is
/// allocated.
///
/// The four items of the card, in the order they act inside one election:
///  1. Held basis: a retained incumbent re-pins at the frozen @c basis value instead of
///     re-deriving it, so the datum is constant for the whole tenure.
///  2. Every handover declared: a new election records @c prev_value / @c prev_station
///     in its entry, which is the Δ item 2 declares to the consumer.
///  3. Election: when no earlier branch (sticky incumbent, candidacy pass, re-entry
///     restore) has chosen a station, the candidates that pass the arc-age gate
///     (@c min_age_ep) and the screen-flag quiet window (@c flag_quiet_ep) are ranked
///     by descending elevation.  The highest one whose carried level passes the
///     handover ledger verdict is elected; with none passing it, the highest eligible
///     one is elected (@c n_elev_fallback).  With no eligible candidate the
///     coverage-ranked sweeps elect as they do unarmed.
///  4. T4 on the anchor: @c flag_ep holds the last epoch each arc was flagged by any
///     screen.  It is the quiet-window input and also the trigger that deposes a basis
///     holder at the election, before the response acts on its column.
struct PhaseHeldBasis {
    bool      on = false;             ///< network.phase_datum_held_basis, already ANDed with float_kf.
    int       min_age_ep = 0;         ///< network.phase_anchor_min_age_ep (0 = no age gate).
    int       flag_quiet_ep = 0;      ///< network.phase_anchor_flag_quiet_ep (0 = no quiet gate and no flag deposition).
    long long now_ep = 0;             ///< ctx.counters.n_loop_epochs: the tenure and age clock.
    /// The basis ledger, in/out: ambiguity key -> the frozen basis.  Owned by
    /// @c NetworkCarryState so it survives the epoch.  The election reads it to hold a
    /// retained incumbent's value and writes a new entry on every other election.
    std::map<std::string, PhaseBasisEntry>* basis = nullptr;
    /// Per station, this epoch's LOS elevation by satellite (degrees): the election's
    /// rank key.  A null map, or a missing station or satellite, reads as −1 and ranks
    /// last; with no elevations at all the rank reduces to coverage order among the
    /// eligible candidates.
    const std::vector<std::map<std::string, double>>* elev = nullptr;
    /// (station idx, ambiguity key) -> the loop epoch of its last screen flag (slip
    /// response, T4 / T3 refloat, gross-LOS declaration).  Null: no quiet-window gate
    /// and no flag deposition.
    const std::map<std::pair<int, std::string>, long long>* flag_ep = nullptr;
    /// The phase-leveling carry.  The arc-age gate reads @c LocalCarry::birth_ep on the
    /// restored-arc convention (@ref localCarryYoung), so a warm-resume-restored entry
    /// reads mature instead of being refused for min_age epochs after every resume.
    /// An arc with no carry entry is refused as newborn.  Null: no age gate.
    const net::ArcTable* carry = nullptr;   ///< The arc table (its @c ArcState::amb).
    // ---- write-back (per call; the driver sums them into the run-end readout) ----
    long long n_held = 0;             ///< families re-pinned at their frozen basis value this epoch.
    /// New basis entries written: every election that did not re-pin a retained
    /// incumbent at its frozen value (first sightings, handovers, and a retained
    /// incumbent with no matching entry).
    long long n_elected = 0;
    long long n_elected_cold = 0;     ///< …of which were cold handovers (value 0; the family stepped).
    long long n_refused_young = 0;    ///< candidate evaluations refused by the arc-age gate.
    long long n_refused_flagged = 0;  ///< candidate evaluations refused by the screen-flag quiet window.
    /// Elections whose elevation rank picked a different station than the coverage
    /// sweep would have, plus one per flag deposition (@c n_depose_flag).
    long long n_rank_changed = 0;
    /// Retained-incumbent epochs whose sticky R2 target differed from the frozen basis
    /// value; the frozen value is pinned.  This is the held property's own check, and
    /// any nonzero value is a defect.
    long long n_value_drift = 0;
    // ---- Deposition and fallback causes (RF-2b): one counter per cause, so a run
    //      shows why the datum moved and not only how often the basis changed.
    /// Candidate evaluations in the elevation rank refused because the continuous
    /// branch's own ledgerVerdict does not accept the candidate's carried level as a
    /// datum: no ledger entry, a non-finite value, a variance that is non-positive,
    /// non-finite or over network.phase_datum_handover_max_var_m2, the ratio ceiling,
    /// or a forced-cold directive.
    long long n_refused_unusable = 0;
    /// Elections where no candidate passed the ledger verdict and the rank fell back
    /// to elevation only.  The pin is then not continuous (a cold handover, or a 0-pin
    /// on a first sighting), which is a different event from a clean handover.
    long long n_elev_fallback = 0;
    /// Incumbents deposed by a screen flag on their own basis column (item 4's
    /// trigger), never by a flag elsewhere at that station.
    long long n_depose_flag = 0;
    /// Incumbents that no earlier branch kept or replaced: absent, no longer tracking
    /// the key, banned by a directive, or deposed by a basis flag (so also counted in
    /// @c n_depose_flag).  Counted only when the elevation rank runs for the family,
    /// that is when its constellation has candidates.
    long long n_depose_arc_death = 0;
    /// Always 0.  The incumbent's own ledger verdict is not tested: a gauge-pinned
    /// column is excluded from the carry (the S-basis contract), so the handover ledger
    /// has no live entry for the station holding the basis.  Nothing increments this
    /// counter; it is kept so a build can show that it reads 0.
    long long n_depose_unusable = 0;
};

/// Coverage-gate drop-edge bundle (@c network.phase_datum_gate_drop_erase;
/// the rule itself is @ref networkPhaseGateDropEdges in network/solve/PhaseGateDrop.hpp).
///
/// The driver passes it on every run, armed or not, because requirement F2 asks the
/// unarmed run to report the size of the class (standing invariant #4: a counter that
/// exists only on the armed path cannot say whether arming would matter).  Unarmed, it
/// is telemetry only: @ref events is filled, @c reref_out is not touched, and the solve
/// is byte-identical.
struct PhaseGateDrop {
    /// network.phase_datum_gate_drop_erase.  false: the edges are detected and
    /// counted, but the family is not added to the re-reference set.
    bool on = false;
    /// [out] one entry per drop edge found this epoch, in ambiguity-key order.
    /// The driver turns these into @c PHASE_ANCHOR / @c gate_drop trace rows and
    /// folds the counts into the run-end readout; the election neither reads them
    /// back nor lets them reach the solve.
    std::vector<PhaseGateDropEvent> events;
};

/**
 * @brief Cold anchor-fallback inputs (@c network.phase_datum_candidacy_usable,
 *        docs/SPEC_event_harness.md §15): a read-only view shaped like
 *        @ref AnchorHygiene, so an election handed a default instance behaves as it
 *        does without the card.
 *
 * The election's sweep takes the first coverage-ranked tracker of the key; the
 * continuous branch then looks that successor up in the handover ledger and may
 * refuse it, which pins at 0 and re-realizes the family's gauge.  These members let
 * the sweep ask the refusal question first, of every candidate, in the same coverage
 * order:
 *
 *   * the ledger is already a parameter of the election (@p handover), as are both
 *     variance ceilings.  The predicate is shared with the continuous branch (one
 *     lambda, two readers), so the screen cannot drift from the decision it previews;
 *   * @ref free_ambs / @ref slipped_ambs are this epoch's freed sets.  A candidate
 *     whose own arc a slip response just freed is about to have its level re-drawn
 *     from one epoch of code, so its carried value is not a datum however small its
 *     variance (the re-coupling edge of standing invariant #1, applied at candidacy);
 *   * @ref local_carry and @ref now_ep supply the arc age (@c LocalCarry::birth_ep),
 *     the maturity bar with which the §15 census separated the cold and continuous
 *     populations.
 */
struct AnchorCandidacyUsable {
    /// network.phase_datum_candidacy_usable, already ANDed with the handover
    /// mechanism being armed.  false: neither the pass nor the satellite clause runs.
    bool on = false;
    /// network.phase_datum_candidacy_min_age_ep (0 = no age test).
    int  min_age_ep = 0;
    /// Iteration 2, the satellite clause (network.phase_datum_candidacy_satellite,
    /// SPEC_event_harness §15.8).  false: iteration 1's per-family screen, which the
    /// single-knob A/B needs.  true: the families of a satellite that hand over in the
    /// same epoch take one successor, and a partial handover prefers the station
    /// already anchoring the satellite's other families.  The per-family screen
    /// improved producer quality but referenced one satellite's bands to different
    /// stations, which the consumer pays for (§15.6).
    bool satellite_clause = false;
    /// The loop-epoch counter the carries' @c birth_ep is stamped from
    /// (`ctx.counters.n_loop_epochs`); age = now_ep − birth_ep.
    long long now_ep = 0;
    /// `ctx.carry.relevel.free_ambs`: the leveling free-epoch latch (structurally
    /// empty on carry_invariant lineages).  Null: no freed test from this vehicle.
    const std::set<std::pair<int, std::string>>* free_ambs = nullptr;
    /// This epoch's DD invariant-carry slip funnel, same encoding; the vehicle on
    /// carry_invariant lineages.  Both sets are consulted.
    const std::set<std::pair<int, std::string>>* slipped_ambs = nullptr;
    /// `ctx.entities.arcs`, keyed (station index, sat, signal), read only for
    /// @c birth_ep through @ref localCarryYoung.  An unknown age counts as mature (the
    /// restored-entry convention): a null map, no entry for the arc, or a
    /// non-positive @c birth_ep, meaning never stashed (0) or warm-resume-restored
    /// (@ref LocalCarry::kBirthEpRestored).  The age test then passes and the ledger
    /// verdict and the freed test decide.
    const net::ArcTable* local_carry = nullptr;   ///< The arc table (its @c ArcState::amb, N4f1).
};

/**
 * @brief Prioritized per-(sat,signal) anchor election with stickiness.
 *        Relocated unchanged at INC-8b.
 *
 * Keeps the sticky incumbent while it is present, still tracks the key, and is
 * neither banned by a directive nor deposed by a held-basis flag (fewest handovers).
 * Otherwise the successor comes from the first of these branches that yields one:
 * the candidacy pass (@p cand_usable), the re-entry restore (@p reentry, first
 * sightings only), the held-basis elevation rank (@p held), then the coverage-ranked
 * sweeps over the constellation's trackers.  Most of a constellation then sits on one
 * receiver (frame-consistent), with per-satellite fallback.  The pin is written to
 * the anchor's own pinned_amb_targets (its local block, which the datum depends on).
 * Its target is 0.0 except for a continuous handover (the successor's ledger value),
 * a retained incumbent with a sticky R2 target in @p anchor_pin_target, and a
 * retained incumbent re-pinned at its held-basis value.  A key with no tracker is
 * left out of @p shared_phase_keys, so every shared key other than a bridged one
 * (see @p hyg) has exactly one pin.  A key whose pin still fails to fold is
 * orphaned, which is a case @ref healNearNullPhaseBias exists to catch.
 *
 * @param sticky            network.phase_datum_sticky (passed as a scalar so
 *                          this header needs no GnssConfig definition).
 * @param hyg               eq34 reattach hygiene verdicts (see
 *                          network/solve/AnchorHygiene.hpp).  Default-constructed
 *                          (inactive), it leaves the election unchanged.  When
 *                          active, it makes a converged candidate outrank a fresh
 *                          one whenever the sticky incumbent is not kept (keeping it
 *                          is not a reattach), leaves a bridged key in the shared set
 *                          with no pin (its soft shared row is the datum), and
 *                          supplies the INC-3 σ multiplier for an unavoidable fresh
 *                          pin.
 * @param tally             [in/out] run counters (reattaches, cold fallbacks,
 *                          ramped pins, candidacy and satellite-clause splits,
 *                          handover outcomes).  Pass @c ctx.counters.hyg.
 * @param anchor_incumbent  [in/out] sticky baseline; swapped at the end.  Must
 *                          be the run's ctx.carry instance: a copy degrades the
 *                          election to a fresh one every epoch, which shows only
 *                          as handover churn.
 * @param this_epoch_anchor [out] read at the emit site for handover signalling.
 * @param handover          Two-edge continuous handover
 *                          (docs/SPEC_pivot_visibility_gauge_defect.md §5.1).
 *                          @c nullptr (the default) is the election without the
 *                          card: every pin target is 0.0.  When supplied, a
 *                          handover pins the incoming anchor at its own absolute
 *                          ambiguity estimate from the ledger, which is its value
 *                          in the outgoing frame, so the gauge change is an
 *                          S-transform and the emitted (sat,signal) family does
 *                          not step.  A successor with no ledger entry (a fresh
 *                          arc, or the first epoch), or one refused by the
 *                          variance ceilings @p max_var and @p max_var_ratio,
 *                          still pins at 0 and is counted as a cold handover: an
 *                          unavoidable re-realization declared to the consumer is
 *                          a different event from a silent one.
 *                          R2 (§5.09): a continuous target is sticky.  It is
 *                          stored in @p anchor_pin_target and re-applied every
 *                          epoch the incoming anchor remains incumbent, so the
 *                          inherited frame persists for the anchor's whole
 *                          tenure instead of reverting to 0 at T+1 (per §5.07.3
 *                          a one-epoch target only defers the step to T+1).  A key
 *                          with no stored target keeps its 0.0 pin, so a settled
 *                          network that never handed over is byte-identical even
 *                          with the ledger supplied.
 * @param handovers_out     [out, optional] per-handover attribution
 *                          (@ref AnchorHandoverEvent), appended, for the caller's
 *                          @c trace_events rows; one record holds both edges.
 *                          Observational only: it does not change which station is
 *                          elected or what target is pinned.  Filled whenever it
 *                          is non-null, including when @p handover is @c nullptr,
 *                          so the churn of an unarmed arm can be measured before
 *                          the fix is armed.
 * @param reref_out         [out, optional] queue item 1(d)
 *                          (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1):
 *                          the (sat, signal) families, inserted, whose pin this
 *                          epoch is not continuously inherited from an incumbent,
 *                          so their carried state may belong to a retired frame.
 *                          Three classes are inserted: every cold handover; every
 *                          pin with no incumbent (a first sighting, or a bridge
 *                          exit, whose incumbent record lapses while the key is
 *                          bridged); and, with @p gate_drop armed, every
 *                          coverage-gate drop edge.  A continuous handover is not
 *                          inserted (§5.09): under the sticky target the successor
 *                          stays pinned in the outgoing frame for its whole tenure,
 *                          the family does not step at T or T+1, and the carried
 *                          state remains in frame, so erasing it would only
 *                          truncate arcs.  The caller routes the set to
 *                          ctx.carry.phase_anchor_reref, where the D2/C1
 *                          shared-carry consume marginalizes those PB keys for one
 *                          epoch (as for @c eq34_reref) and
 *                          @ref networkApplyPhaseAnchorCarryReref erases the
 *                          families' phase-leveling local carries.  @c nullptr
 *                          (the default, and the knob-off path) is byte-identical:
 *                          nothing is inserted and nothing downstream fires.
 *                          Unlike @p handovers_out this output is not
 *                          observational: its consumers change which priors fold.
 * @param inject        Event-harness directive set (network/carry/Inject.hpp), or
 *                      @c nullptr, which is the normal path and every unarmed run.
 *                      It also carries the anchor-slip handovers staged by
 *                      @ref networkStageAnchorSlipHandovers.  When a staged
 *                      @c anchor_handover covers a family, the incumbent is
 *                      excluded from candidacy for this epoch only (forcing a
 *                      successor).  In @c cold mode the continuous-handover branch
 *                      is refused, so the family takes the re-realize and
 *                      re-reference path.  The directive records which station it
 *                      retired and which was elected.
 * @param cand_usable   Cold anchor fallback (@c network.phase_datum_candidacy_usable,
 *                      docs/SPEC_event_harness.md §15), or @c nullptr / a default
 *                      instance, which leaves the election unchanged.
 *
 *                      When @c on, and only where a handover is about to happen (an
 *                      incumbent exists for the key and the sticky branch did not
 *                      keep it: absent, no longer tracking the key, banned by a
 *                      directive, or deposed by a held-basis flag), the election
 *                      first sweeps the same coverage-ranked list and accepts only
 *                      a candidate whose carried value is usable: the continuous
 *                      branch's own predicate (the shared lambda, not a copy),
 *                      plus "not itself freed this epoch" and the arc-age bar.
 *                      The first usable candidate in coverage order is elected.
 *                      With none usable, the election falls through to its
 *                      remaining branches unchanged (the held-basis rank when
 *                      armed, then the coverage sweeps), counted in
 *                      @c tally.candidacy_no_usable_fallback.
 *
 *                      A key without an incumbent (a first sighting or a bridge
 *                      exit) is not screened: there is no outgoing frame, the
 *                      continuous branch cannot run, and every such pin is a 0-pin
 *                      whichever station takes it, so screening would move
 *                      elections without improving them.  The hygiene eligibility
 *                      test is part of the screen, so the per-family pick is always
 *                      a station the first coverage sweep would also accept,
 *                      possibly a later-ranked one.  With @c satellite_clause on,
 *                      the satellite's joint host overrides the per-family pick
 *                      and needs only to track the key, so that guarantee then
 *                      covers the per-family pick alone.
 *
 *                      Property: with the satellite clause off, a handover goes
 *                      cold only when no hygiene-eligible tracking candidate passes
 *                      the screen, so @c tally.handover_cold then counts the
 *                      handovers the screen could not make continuous.  With the
 *                      clause on, a family may also take a cold pin on the
 *                      satellite's joint host (@c tally.sat_clause_cold_colocated).
 *                      The sticky branch, the bridged branch, the hygiene sweeps,
 *                      the continuous/cold decision and every target computation
 *                      are unchanged; the pass changes only which station reaches
 *                      the continuous branch.
 * @param gate_drop     Coverage-gate drop edge
 *                      (@c network.phase_datum_gate_drop_erase), or @c nullptr, which
 *                      leaves the election unchanged.  When supplied, this epoch's
 *                      coverage-gate drop edges are detected immediately before the
 *                      incumbent swap (the last point at which @p anchor_incumbent
 *                      still describes the previous epoch) and appended to
 *                      @c gate_drop->events.  With @c gate_drop->on and @p reref_out
 *                      non-null, each edge's (sat, signal) is also inserted into
 *                      @p reref_out, so the family takes the path the return edge
 *                      already takes: the D2/C1 shared-carry consume marginalizes
 *                      its PB key for one epoch and
 *                      @ref networkApplyPhaseAnchorCarryReref erases every
 *                      station's carried ambiguity of it before assembly, in the
 *                      same epoch.  Otherwise the surviving stations' carried
 *                      floats absorb the removed bias whole (EV7 §5.3).
 *
 *                      The erase therefore depends on @p reref_out and has no
 *                      effect without @c network.phase_anchor_carry_reref, which
 *                      supplies that pointer.  Both working bases
 *                      (config/refound/{field,sim}_gps_dual_rf0hv.json) arm it.
 *
 *                      A gated key is erased from @c cnd.union_keys before the
 *                      election's loop, so it cannot be a handover or a first
 *                      sighting in the same epoch: the drop-edge marks and the
 *                      §5.07.5 R1 marks are disjoint populations.
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
        const NetworkInjectState* inject = nullptr,
        const AnchorCandidacyUsable* cand_usable = nullptr,
        PhaseHeldBasis* held = nullptr,
        PhaseGateDrop* gate_drop = nullptr,
        /// Anchor re-entry: the ledger read at the
        /// first-sighting branch, and the drop-edge memory the restore decides on.
        /// Null (the default) leaves the election byte-identical; with only its
        /// observe half armed it counts and records events and changes nothing else.
        AnchorReentry* reentry = nullptr);

/**
 * @brief Anchor-slip handover (network.cycle_slip.anchor_slip_handover): stage a
 *        one-epoch candidacy ban for every (sat, signal) family whose anchor arc a
 *        slip response freed this epoch.
 *
 * Each phase-bias family is realized at one station's ambiguity: the anchor
 * @ref networkElectPhaseAnchors elected and keeps electing while it is sticky.  When
 * a detector drain frees that arc, nothing else hands the family over: the sticky
 * election re-elects the slipped incumbent, and the gauge is re-realized from an arc
 * whose level is about to be re-drawn from one epoch of code.  The error then spreads
 * to healthy arcs at other stations and to the station and satellite clocks; a
 * handover in the same epoch avoids it (docs/SPEC_event_harness.md §12.6/§12.7
 * has the measurements).
 *
 * The staging is expressed as the injector's candidacy ban (a directive in
 * @c NetworkInjectState::anchor_now_slip, @c cold = false) rather than a direct
 * assignment, as the harness does, so the successor is chosen by the production
 * rules: coverage rank, hygiene eligibility, the variance gates on the continuous
 * branch, and the cold fallback when none of them can be satisfied.  This function
 * forces the event, not the outcome.
 *
 * Call it after every PASS-A slip drain (both freed sets must be complete) and
 * immediately before @ref networkElectPhaseAnchors, whose @p inject argument must
 * then be non-null whenever this function returned a positive count.
 *
 * @param stations            The station roster; only @c name is read.  Index
 *                            @c i of each freed pair indexes into it.
 * @param anchor_incumbent    amb key ("<sat>_<signal>") -> anchoring station name,
 *                            as it stands before this epoch's election.  A freed
 *                            arc whose station is not this map's entry for its key
 *                            is a non-anchor arc and stages nothing.
 * @param free_ambs           @c ctx.carry.relevel.free_ambs: the latched
 *                            (station index, amb key) set the leveling free-epoch
 *                            response marked (empty unless
 *                            network.cycle_slip.relevel_free_epoch).
 * @param carry_slipped_ambs  The epoch's DD invariant-carry slip funnel, the same
 *                            pairs in the same encoding: the vehicle on
 *                            @c carry_invariant lineages, where the leveling latch
 *                            is structurally empty.  Both are consulted; a family
 *                            named by both stages one directive.
 * @param inject              Run-scope directive state; @c anchor_now_slip is
 *                            appended to (it was cleared at the top of the epoch).
 *                            Nothing else in it is touched: the injector's own
 *                            schedule, ledger and never-fired logic are unchanged.
 * @return The number of directives staged (the families whose anchor arc was freed).
 *         0 means no anchor arc was freed this epoch (or there is no incumbent map
 *         yet, on the run's first epoch).  On a run whose detectors fired, 0 means
 *         the slips did not land on a gauge arc.
 */
int networkStageAnchorSlipHandovers(
        const std::vector<NetStation>& stations,
        const std::map<std::string, std::string>& anchor_incumbent,
        const std::set<std::pair<int, std::string>>& free_ambs,
        const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
        NetworkInjectState& inject);

/**
 * @brief Queue item 1(d): erase the phase-leveling local ambiguity carries of
 *        every re-referenced (sat, signal) family, at every station.
 *
 * The local ambiguities depend on the frame (z ≈ range − bˢ_σ − …), so when the
 * family's phase-bias datum re-realizes at a handover the carried floats are stale in
 * the same way the shared PB carry is; they refloat next epoch.  The same erase as
 * the eq34 D4 carry hygiene (@c ChainDatum.cpp), applied to the anchor
 * election's re-reference set.  Runs at the election site, before PASS B consumes
 * the carry.
 *
 * @param reref        This epoch's re-referenced (sat, signal) families
 *                     (ctx.carry.phase_anchor_reref).  Empty: returns 0 immediately.
 * @param arcs         ctx.entities.arcs: the phase-leveling per-station
 *                     ambiguity carry, keyed (station idx, sat, signal).  Inert
 *                     (empty map) unless network.phase_leveling.
 * @param erased_out   [out, optional] RF-1 §7.4; null on every unarmed run.  Receives
 *                     one (key, entry) pair per erased entry, captured before the
 *                     erase, so @c network_amb_state.csv can carry this class in its
 *                     D3 inventory.  The caller, the only scope that knows the station
 *                     names and the epoch stamp, turns them into rows.  It is an
 *                     out-parameter rather than a context handle so this function
 *                     stays a pure map operation and its unit test can keep calling
 *                     it with two arguments.
 * @return Number of local carry entries erased.
 */
int networkApplyPhaseAnchorCarryReref(
        const std::set<std::pair<std::string, std::string>>& reref,
        net::ArcTable& arcs,
        std::vector<std::pair<std::tuple<int, std::string, std::string>, LocalCarry>>*
            erased_out = nullptr);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the phase datum: the held S-basis (RF-2), the prioritized drop and heal,
 *        the anchored-union gate and its drop edge.
 *
 * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
 * beside it in PhaseDatum.cpp, print in this order, each gated on its own knob:
 *   - networkHeldSbasisRunEnd: the held S-basis (RF-2).
 *   - networkDropHealRunEnd: the prioritized-datum drop-and-heal.
 *   - networkPhaseDatumGatesRunEnd: the anchored-union coverage gate and its drop edge.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0, or the last non-zero verdict of its sections.
 */
int networkPhaseDatumRunEnd(const NetworkRunEndView& v);

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
 * @brief The phase-datum pins of the station's anchored ambiguities and the eq34 receiver
 *        phase-bias rows.
 *
 * Step `phase_datum_pins` of kStationBuildSteps (network/assembly/StationBuild.hpp); its kinds:
 * AmbiguityPin, Eq34RpbPin, Eq34RpbRankFloor, Eq34RpbTimeLink. Records: np, pin_audit,
 * pin_seed_targets, pl_rpb, prm_rpb_gap_skip. The frozen text of networkPassB_'s produce loop
 * below its bindings.
 * @param build One station's build: the station and its slot writable, the run read only.
 * @return StationStep::Next.
 */
StationStep networkPhaseDatumPinRows(StationBuild& build);

/**
 * @brief True when station @p i's (sat, signal) ambiguity is gauge-pinned,
 *        that is, part of the phase datum rather than a free (carriable)
 *        ambiguity (decomposition INC-7).
 *
 * Legacy datum: every pivot-station ambiguity.  Prioritized datum: exactly
 * the elected @c pinned_amb_targets entries.  Implemented in
 * @c ApplicationNetworkAssembly.cpp.
 *
 * The single test that replaced the scattered is_pivot gauge tests: under the
 * legacy datum it returns is_pivot, so every rewritten site stayed byte-identical;
 * under the prioritized datum the "sat_signal" key form matches the pin fold and
 * the ambiguity-state key, and the pin is folded into the station's own local
 * block.  The Eq-34 / de Jonge single-receiver-chain datum drives the same
 * prioritized machinery (no pivot-first, every station carries its arcs,
 * per-(sat,signal) shared set) and diverges only at the election (a spanning-tree
 * chain election) and the pin fold (per-tree-edge ambiguity pins with a target,
 * plus a datum-receiver RPB pin).  (Moved here from runNetwork,
 * where it had stayed beside the phase-datum aliases.)
 *
 * A free function (formerly Application::networkIsGaugePinned_), so the station
 * steps in the family files can call it.
 */
bool networkIsGaugePinned(const std::vector<NetStation>& stations, bool phase_datum_prioritized,
                          size_t i, const std::string& sat, const std::string& signal);

// ---- End of the PASS B station steps, declarations ----

} // namespace GnssProcessor
