#pragma once
/**
 * @file ChainDatum.hpp
 * @brief Eq-34 / de Jonge single-receiver-chain phase-bias datum for the PPP-RTK
 *        network producer.
 *
 * For each connected component of the bipartite (receiver × satellite) observation graph of
 * a family (a constellation's phase signal, or with the pair family the constellation's L1/L2
 * pair; see "Families" at the election), the election picks a datum receiver r1
 * (its receiver phase bias is pinned to 0) and pins the ambiguities on the edges of a
 * sticky spanning tree rooted at r1.  Every between-satellite reference is then an
 * integer combination across the whole component, including satellites r1 does not
 * see, because each chain hop contributes only an integer: the hop receiver's
 * fractional bias is its own estimable RPB state δ_{r,σ} and is not part of the pin.
 * With full visibility (the regional case) the chain reduces to the Odijk Eq-34 star.
 *
 * Gauge bookkeeping, cross-checked every epoch (the RPB allocation rests on it):
 * a component with n_c stations and m_c satellites gets (n_c + m_c − 1) tree-edge
 * ambiguity pins plus 1 RPB pin, n_c + m_c in total, which equals the gauge
 * dimension.  Every RPB-carrying station is a tree vertex, so it gets at least one
 * pin and its local normal block N_rr stays positive-definite.
 *
 * Sibling of network/solve/PhaseDatum.hpp, which owns
 * the candidate census consumed here, the non-chain realization (the anchor
 * election) and the drop-and-heal.  Owns no state: every carry object
 * is a reference into the caller's NetworkRunContext.
 *
 * Ordering contract: this function mutates persistent carries before the solve.  It
 * erases the arcs' leveling carries (ArcState::amb) for re-referenced (sat,σ), erases
 * and shifts the receiver phase-bias carries (StationSignalState::rpb), shifts
 * prev_carry PhaseBias entries, and fills events.phase_datum.eq34_reref (read
 * downstream by networkInjectD2CarryPrior, networkStashCarry and the carry
 * variance-component monitor's shared-prior snapshot).  It therefore runs:
 *   - after networkAssemblyPrefilter_ (its phase-bias prune is a precondition);
 *   - before networkReceiverPhaseBiasAllocation.  This
 *     function is the only writer of shared_phase_keys on the eq34 path, and the
 *     allocation derives this epoch's need_rpb set from it.  If the allocation ran
 *     first it would allocate and prune RPB states against the previous epoch's shared
 *     set: this epoch's pinned_rpb_keys could name just-pruned states, and stations
 *     newly entering the observation graph would get no δ_{r,σ} column.  The rank
 *     cross-check counts recorded pins, not allocated columns, so it would not report
 *     this; the defect would show only as null-space-gate withholds and non-PD local
 *     blocks;
 *   - and hence before est.reset() / est.beginEpoch() (inside
 *     networkAssemblyRegisterColumns_), networkPassB_ and networkStashCarry.
 * Inside the function, the extend-only merge runs before the sticky baselines are
 * replaced (the assignment of `anchor_incumbent` from `this_epoch_anchor`).
 */

#include "network/driver/RunTypes.hpp"   // NetStation / PhaseDatumCandidates
#include "network/solve/AnchorHygiene.hpp"   // AnchorHygiene (eq34 reattach hygiene)
#include "network/state/CarrySites.hpp"      // CarrySiteCounts

#include <array>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <string>
#include <vector>
#include <cstddef>

namespace spdlog { class logger; }

namespace GnssProcessor {

struct EpochEvents;   // network/driver/Epoch.hpp

class GnssConfig;
struct NetworkRunContext;   // network/state/RunContext.hpp, included in the .cpp only

/**
 * @brief The eq34 chain datum's carry shift through the traversal of carried references
 *        (@c CarryMechanism::Eq34Shift).
 *
 * The rule of the apply: a carried ambiguity moves by
 * @f$-\Delta b_s - \Delta\delta_r@f$ (its satellite's and its receiver's chain steps, formed in
 * that order and added once), a carried receiver phase bias by its own
 * @f$\Delta\delta_r@f$, a carried satellite phase bias by @f$\Delta b_s@f$, and the s_prev of
 * every phase-bias coupling term on a shifted satellite column (in the ambiguity, receiver
 * phase-bias and wet-delay carries) by @f$\Delta b_s@f$.  A non-finite or zero step moves
 * nothing.
 *
 * With @p complete (@c network.carry_rebase_complete) the walk also moves
 * the sites the mechanism table exempts as owed, each by the same rule: the gross screen's
 * held ambiguity like the ambiguity (@f$-\Delta b_s - \Delta\delta_r@f$), the last solved
 * phase bias (@c pb_level_prev, "SAT_SIGNAL") like the carried phase bias
 * (@f$+\Delta b_s@f$), and the phase-bias coupling terms on a shifted satellite column in
 * every other coupling map (the slant ionosphere and its block, the gross screen's three
 * holds, the receiver clock and code bias, the phase colour) by @f$\Delta b_s@f$.
 * The phase hold's held value (an emitted value) and an open pin bridge (a root of the chain
 * with a zero shift) are not moved.
 * @param carry     the run's carried state.
 * @param entities the run's entity tables (the station carries).
 * @param shift_sat (satellite, signal) -> @f$\Delta b_s@f$.
 * @param shift_rxg (station, constellation, signal) -> @f$\Delta\delta_r@f$.
 * @param shift_rpb (station, receiver phase-bias key) -> @f$\Delta\delta_r@f$.
 * @param n_values  out: carried values moved (ambiguities, receiver and satellite phase
 *                  biases; the held ambiguity and the last solved phase bias moved under
 *                  @p complete are counted in @p counts only).
 * @param n_terms   out: coupling terms moved.
 * @param counts    optional out: per-site tallies (accumulated).
 * @param complete  also move the owed sites (false = the walk without them).
 */
void eq34ShiftCarry(NetworkCarryState& carry, net::NetworkEntities& entities,
                    const std::map<std::pair<std::string, std::string>, double>& shift_sat,
                    const std::map<std::tuple<int, char, std::string>, double>& shift_rxg,
                    const std::map<std::pair<int, std::string>, double>& shift_rpb,
                    long long& n_values, long long& n_terms,
                    CarrySiteCounts* counts = nullptr, bool complete = false);

/**
 * @brief Elect the eq34 chain datum for this epoch.
 *
 * Resets every station's pins, seeds the shared set from the (post-gate) union,
 * builds the bipartite graph of each family, elects r1 and grows a
 * sticky spanning tree per connected component, then runs the carry hygiene
 * (re-reference erase + S-transformation apply + extend-only merge-back) and
 * advances the sticky baselines.
 *
 * The graph, its components and the tree are those of network/ar/ArcGraph.hpp:
 * arcgraph::buildArcGraph per family (stations in roster order,
 * satellites in string order, arcs in usable_keys order), arcgraph::arcComponents in
 * the legacy order (arcgraph::legacyChainVertexRank) and arcgraph::growChainForest per
 * component (roots r1 and the bridged satellites, the live incumbent arcs as the
 * sticky seed).  The pins are recorded by replaying the tree's edges in attachment
 * order after the walk, which is exact because the walk reads nothing a pin writes.
 *
 * ## Families
 *
 * The unit of the election is a family.  By default (@c network.phase_datum_eq34_family
 * "signal") every (constellation, phase signal) group is a family of one band.  With "pair" a
 * constellation that tracks exactly two phase signals of different bands in the epoch forms one
 * family of both: one graph whose arcs carry a presence bit per band, one forest per component
 * of the arcs both bands carry, so the same tree edges are pinned on both bands, and one datum
 * receiver per component, pinned on both bands' receiver phase-bias keys.  The pins, the frame
 * steps and the observatory's records stay per band.  A vertex only one band tracks (a station or
 * satellite none of whose arcs both bands carry) is attached afterwards, on its band alone, to the
 * pair tree its band's graph reaches it from (the band extension); a component of one band's graph
 * with no vertex in any pair tree is elected on that band as a one-band component.  Any other
 * constellation is elected per signal in that epoch.  The pin bridge grounds a satellite per
 * (satellite, signal) key, which one shared tree cannot hold: the loader refuses it with the
 * pair family, and a pair family is never rooted at a bridged satellite.
 *
 * ## Event rows
 *
 * The frame is the set of (arc, target) pins plus the receiver phase-bias pin of each root; it does
 * not depend on the direction of an arc.  The as-built rule keys a tree edge on its child's
 * incumbent parent, so an arc that stays in the tree with its direction reversed (a fragment that
 * re-attaches through one new edge reverses the path to its old top) reads as a re-host, and it
 * re-reads its target from the arc's leveling carry, which a pinned arc never refreshes: a stale
 * chord carry, then nothing, so the target falls back to 0.  With the kept row armed
 * (@c network.phase_datum_eq34_event_rows "kept") an incumbent tree edge, in either direction,
 * whose pinned life continues (@c arcPinnedLifeContinues against the stamp remembered in
 * @c ArcState::eq34_pin_life), that is not in the epoch's free-epoch latch, whose satellite does
 * not remember another root and that has a previous target, is held: it keeps its previous target
 * and takes no frame step of its own, so the shift walk gives its child the parent's step exactly
 * and an unknown parent step passes down.  An edge the row does not hold runs the as-built rule.
 *
 * With the returning row armed (@c "returning") a tree edge whose child vertex rejoins the tree
 * after an absence takes the cut release.  The child is absent from the last election's tree on
 * the band (a satellite with no incumbent anchor; a receiver with no incumbent parent that was
 * not a root, the band's remembered datum receiver or a satellite's remembered root), was a
 * vertex of an earlier election's tree (@c eq34_tree_seen, so a vertex never seen is a birth and
 * keeps the as-built rule) and the arc is not in the epoch's free-epoch latch (that is the slip
 * row's case).  The moved side M is the vertex and its subtree in this election's tree.  The
 * release takes the first-sighting target (0, or the seed round's) with an unknown step in place
 * of the as-built draft, which may have read the target from the arc's old-frame carry; the
 * unknown step re-references the satellites of M and drops the receiver phase-bias carries of
 * its receivers, and the release then erases every leveling carry that touches M (a satellite of
 * M at any station, a receiver of M to any satellite on the band, at present and absent
 * stations) before the shift walk applies.  The keys of M's satellites are the moved keys the
 * observatory declares.  The row certifies nothing: a returning vertex's own carries are never
 * trusted to keep its old frame, so every returning edge takes the release.
 *
 * With the rehost row armed (@c "rehost") a tree edge whose child had a parent at the last
 * election but whose arc was no tree edge then, in either direction, takes the same cut release.
 * The arc is not in the epoch's free-epoch latch (that is the slip row's case).  The pin keeps the
 * target the as-built rule drafted, which is already an integer (the rounded carry of the new host
 * or the seed round's first-sighting target), with an unknown step: the integer of a chord the
 * previous epoch certified is not read, so every re-host takes this rule.  The release is the
 * returning row's: the moved side is the child and its subtree, whose satellites are
 * re-referenced and declared moved and whose receivers lose their carries.  A held or flipped
 * kept edge below a released re-host joins its moved side.
 *
 * With the root row armed (@c "root") a component whose datum receiver is new pins the new root's
 * receiver phase bias at the value its last solve gave it, in place of 0.  In a chain frame the
 * root's pin is a constant the S-basis leaves free, and as built it is 0, so a new root's real bias
 * becomes a step of the whole component (marked unknown, every satellite of the component
 * re-referenced).  Pinned at its carried value the frame every other carry is in does not step.  The
 * row applies to a handover when: some satellite of the component remembers a root other than the
 * new one; the new root had a parent edge in the last tree on the band (so the old root is known);
 * the component is the piece that keeps the old root (the piece holding the old root's station, else
 * the largest piece, as the observatory reads a split); and the new root's receiver phase-bias
 * carry is no older than the carry horizon.  A satellite edge whose satellite remembers the old
 * root is then not marked unknown on that account (it keeps its carries); a satellite that remembers
 * any other root (a merge) stays unknown.  A root that stays a root keeps its pinned target (the
 * station's @c eq34_root_rpb_target), because a pinned bias is not refreshed by the carry.  Every
 * other handover runs the as-built rule and is counted by cause.
 *
 * With the slip row armed (@c "slip") an arc in the epoch's free-epoch latch
 * (@c RelevelEpochState::free_ambs, the arcs a slip detector or a re-entry response flagged, with
 * their cause bits) leaves the tree at the slip epoch.  An incumbent tree edge on such an arc loses
 * its seat and a new edge on one is refused while another route exists; in a pair family this holds
 * when either band's arc is latched, because the bands share one tree.  The old child re-attaches
 * through a new edge, which is that edge's own row's: the rehost row takes it, its certified branch
 * held, so the old child's moved side takes the cut release, and with the rehost row off it runs the
 * as-built rule.  When no other route reaches the child, the slipped arc stays and the slip row
 * releases it, on the band whose arc is latched: the moved side is the child and its subtree, and the
 * pin takes the first-sighting target for a returning child, the arc's previous target for an arc
 * that was a tree edge at the last election, else the as-built draft.  A child never seen in a tree
 * before is a birth and keeps the as-built rule.
 *
 * With the merge and split row armed (@c "merge_split", which needs @c "root") the graph's component
 * events keep the frame or declare its move.  At a split, a piece that does not keep the old root takes
 * the root row's rule: its new root's receiver phase bias is pinned at the carried value, so the piece's
 * frame does not step and its carries stay.  At a merge, a tree edge whose child was in another tree of
 * the last election than its parent's side (the memory @c eq34_tree_root, written only with the row armed)
 * is released, whichever other row its arc would fall under: the two trees' frames differ by a real number
 * no carried value relates, so the moved side, the child and its subtree, takes the cut release, and the
 * pin keeps the target the as-built rule drafted with an unknown step.  The edges inside that side keep
 * their targets through the kept row: its integers stand, and the release moves them with it.  The datum
 * receiver is elected by the satellites' memory, so the side with more satellites keeps its root and the
 * other is absorbed.  With a row off nothing here changes.
 *
 * @param ctx           [in/out] carry + counters.  The whole context is passed
 *                      instead of about twenty individual references because the
 *                      body value-mutates the leveling carries, the receiver
 *                      phase-bias carries and prev_carry, all of which are copyable:
 *                      a single missing `&` in an enumerated parameter list would
 *                      compile and silently drop the S-transform.
 * @param config_       networkPhaseDatumEq34Receiver / Eq34PinRound.
 * @param log           "main_log": the pin/gauge-mismatch and non-canonical-name
 *                      warnings + 2 debug lines.
 * @param stations      [in/out] pinned_amb_targets / pinned_rpb_keys.
 * @param present       This epoch's presence mask.
 * @param cnd           Candidate census (sat_el / usable_keys / cover / union_keys).
 * @param hyg           eq34 reattach hygiene verdicts for this epoch
 *                      (network/solve/AnchorHygiene.hpp).  Default-constructed
 *                      (inactive), the election is unchanged from the pre-hygiene
 *                      behavior: every arc is an eligible host, nothing is bridged
 *                      and no pin is σ-scaled.  When active it (a) restricts which
 *                      arcs may become new tree edges, (b) adds spanning-forest
 *                      roots (a bridged satellite is grounded by its shared
 *                      pseudo-obs row instead of a parent edge) and (c) supplies the
 *                      σ multiplier for pins the eligible pass could not avoid.
 * @param shared_phase_keys [out] the post-gate union's phase-bias keys.
 * @param events The epoch's events; writes phase_datum.
 * @param now_s  This epoch in continuous GPS seconds (the root row's carry age); 0 when omitted.
 */
void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            const AnchorHygiene& hyg,
                            /// This epoch's complete slip funnel as (station index,
                            /// ambiguity key).  Read only under
                            /// network.phase_datum_eq34_slip_aware_election, to keep a slipped
                            /// arc out of the spanning tree where the graph allows it (a
                            /// slipped tree edge refloats every fundamental cycle through it; a
                            /// slipped chord costs one DD).  The caller must not insert into
                            /// the funnel after this call; the election already runs after
                            /// every PASS-A drain, signal integrity and measurement
                            /// admission.
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys, EpochEvents& events,
                            /// This epoch in continuous GPS seconds (@c EpochClock::pl_now_s).  The
                            /// root row reads it for the age of a receiver phase-bias carry.  The
                            /// default 0 serves callers whose carries carry no time.
                            double now_s = 0.0);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the eq34 chain datum: re-references, pin audit, seed round, slip-aware election.
 *
 * The eq34 chain datum: the re-reference totals, the pair family, the kept row, the returning row,
 * the rehost row, the root row, the slip row, the pin audit, the seed round and the slip-aware election.
 *
 * Gate: each section under its own condition:
 * `config_.networkPhaseDatumEq34()`; `config_.networkPhaseDatumEq34SeedRound()`;
 * `config_.networkPhaseDatumEq34SlipAwareElection()`; the pair family line under
 * `ctx.knobs.eq34_pair_family`; the kept row lines under `ctx.knobs.eq34_row_kept`; the
 * returning row lines under `ctx.knobs.eq34_row_returning`; the rehost row lines under
 * `ctx.knobs.eq34_row_rehost`; the root row lines under `ctx.knobs.eq34_row_root`; the slip row
 * lines under `ctx.knobs.eq34_row_slip`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkChainDatumRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct EpochPassA;              // network/driver/Epoch.hpp
struct EpochAssembly;           // network/driver/Epoch.hpp

// ---- PASS B serial stages, declarations ----

/**
 * @brief The eq34 chain-pin audit: declared, folded, landed and short pins per group, and the
 *        seed-round targets of accepted stations for the next election.
 *
 * No-op unless NetworkKnobs::phase_datum_eq34 and the epoch was not aborted. Called by
 * networkPassB_ after the consume.
 *
 * @param run The run (network/driver/Run.hpp). Bindings: run.ctx (ctx), run.setup (setup).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.events.phase_datum
 *           (phase_datum_ev).
 * @param assembly The station blocks, read (pin_audit, pin_seed_targets, est_index_of), passed
 *                 because networkPassB_ still writes the group. Bindings: assembly.asm_res
 *                 (asm_res), assembly.est_index_of (est_index_of).
 * @param epoch_ok The consume's verdict (kept).
 */
void networkEq34PinAudit(NetworkRun& run, const NetworkEpoch& ep, const EpochAssembly& assembly,
                         bool epoch_ok);

// ---- End of the PASS B serial stages, declarations ----

// ---- Harvest half, declarations ----

struct NetworkRun;                          // network/driver/Run.hpp
struct NetworkEpoch;                        // network/driver/Epoch.hpp
struct HarvestBorder;                       // network/carry/LocalCarryHarvest.hpp
struct StationHarvestBlock;                 // network/carry/LocalCarryHarvest.hpp

/**
 * @brief Eq34: each receiver phase bias with its diagonal conditional variance (no
 *        coupling, by design), then the prune. No-op unless NetworkKnobs::eq34_rpb_q > 0
 *        (network.phase_datum_eq34_rpb_q_m2s; forced to 0 when eq34 is off or carry_invariant is on).
 *
 * @param run The run: its context (carry, counters, knobs), setup (stations) and engines.
 * @param ep This epoch after the solve (the solution, the assembly, the columns and the clock are
 *           read; nothing is written).
 */
void networkHarvestRxPhaseBias(NetworkRun& run, const NetworkEpoch& ep);

// ---- End of the harvest half, declarations ----

/// @brief Run-scope counters of ChainDatum, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct ChainDatumCounters {

    // Slip-aware eq34 election (network.phase_datum_eq34_slip_aware_election).
    // All three are needed to read the mechanism, because two of them are the policy
    // working and the third is the graph refusing it:
    //   * deseeded:   slipped incumbents denied their sticky tree seat;
    //   * demoted:    slipped arcs whose arcLife ranking was pushed below the clean
    //                 band (inert unless the longevity election is armed); each slipped
    //                 arc counts once per epoch;
    //   * readmitted: slipped arcs that became tree edges anyway, because the
    //                 component had no other route to the vertex.  Those still
    //                 mass-refloat, and a large share here means the topology, not the
    //                 election, is the binding constraint (so "deseeded > 0" does not
    //                 mean "no tree edge slipped").
    long long eq34_slip_deseeded   = 0;
    long long eq34_slip_demoted    = 0;
    long long eq34_slip_readmitted = 0;
    // Run tallies of the chain-pin audit, keyed "<cons>|<signal>" (the election's dr_key).
    // A pin can leak silently from a three-stage pipeline: declared (the election's
    // per-group Σ (n_c + m_c) gauge dimension), folded (foldAmb/foldRpb built the 1e10
    // row; the lambdas return early on a missing state index or a missing live phase
    // row), and landed (the folded row sits on a station that est.addStation accepted;
    // a rejected or dropped station takes every folded pin down with it, the loss mode
    // network.station_pd_pin_aware exists to prevent).  A group whose landed <
    // declared − bridged in any epoch ran with an under-constrained datum that reads
    // like a working one.  Accumulated serially after PASS B's
    // consume; logged once by networkChainDatumRunEnd.  All zero unless
    // network.phase_datum_eq34.
    struct Eq34PinAudit {
        std::map<std::string, long long> declared;         ///< Σ per-epoch gauge dim (n_c+m_c)
        std::map<std::string, long long> bridged;          ///< Σ hygiene bridge pins (shared-side)
        std::map<std::string, long long> folded;           ///< pins foldAmb/foldRpb built (all producing stations)
        std::map<std::string, long long> landed;           ///< folded pins on accepted stations
        std::map<std::string, long long> miss_state;       ///< fold refused: state index absent
        std::map<std::string, long long> miss_row;         ///< fold refused: no live phase row (h2local miss)
        std::map<std::string, long long> dropped_station;  ///< folded pins lost with dropped/rejected stations
        std::map<std::string, int>       short_epochs;     ///< epochs with landed < declared − bridged
        std::map<std::string, int>       worst_short;      ///< max per-epoch shortfall
        int epochs = 0;                                    ///< audited (consumed, non-aborted) epochs
    } eq34_pin_audit;
    // Run tallies of the seed round.  The audit asserts |pin innovation| ≤ λ/2
    // at all times, so a max_abs_y above λ_max/2 is a logic error.  `pins` counts
    // election-side targets (the state existed at election time); `deferred` counts
    // fold-computed targets (state allocated in PASS B: cold start and new arcs);
    // `target_steps` counts same-station target changes between epochs (the reref
    // pressure this knob generates; about 0 expected on static receivers).  All zero
    // unless network.phase_datum_eq34_seed_round.
    long long seed_round_pins         = 0;
    long long seed_round_deferred     = 0;
    long long seed_round_frozen       = 0;   ///< targets reused verbatim from eq34_prev_target, so the frame is realized once
    long long seed_round_target_steps = 0;
    double    seed_round_max_abs_y_m  = 0.0;
    // Re-host tallies.  `framed` = re-hosts whose frame step was computed
    // from the new host's own state and therefore stayed shiftable (the carried PB
    // prior is S-transformed, not marginalized); `cold` = re-hosts that could
    // not be framed (no allocated state: the fold-deferred path), which keep the legacy
    // unknown-step reref.  `max_step_m` is the largest |frame step| any framed re-host
    // applied.  It is bounded by λ/2 only when the new host's float is
    // frame-consistent, so a large value signals that the solution at the new host had
    // drifted off the realized frame: read it as a diagnostic of the runaway the audit
    // probes, never as a quality bound.
    long long seed_round_rehost_framed  = 0;
    long long seed_round_rehost_cold    = 0;
    double    seed_round_rehost_max_step_m = 0.0;

    // One-solve frame skips: the frame stepped, so the carried value is stale in the
    // old gauge; the same epoch's harvest re-stashes any still-tracked arc, so the
    // cost is one solve of continuity.
    long long n_cr_eq34_reref         = 0;  ///< eq34 chain-datum re-reference
    long long n_cr_rpb_gap_prune      = 0;  ///< receiver-PB carry entries dropped by the pl_max_gap_s prune
    long long n_resume_datum_first_reref    = 0; ///< PHASE_DATUM/eq34_reref (sat,σ) at the first post-resume epoch
    // Held phase-bias emissions (pb_hold) of a (satellite,
    // signal) family that a phase-bias shift moved (the eq34 shift, the declared anchor
    // handover) and that stayed in the old frame: PbHold is exempt from both shifts with or
    // without network.carry_rebase_complete.  Measure-only.
    long long prm_pbhold_unmoved_at_shift = 0;
    // Run-scope observability counters of the eq34 datum, logged once in the run-end producer
    // summary (the per-epoch reref/erase lines are debug-only, so these totals are the
    // only non-debug record of the reref volume).  Untouched on non-eq34 runs.
    long long eq34_reref_total = 0, eq34_erase_total = 0;
    long long eq34_shift_total = 0;   // carry values S-transformed over the run
    long long eq34_cpl_sprev_total = 0;   // coupling s_prev references re-based with those shifts; structurally 0 with coupling off, since the stashes are empty
    long long eq34_gap_repin_total = 0;   // tree edges resumed from a carry-alive gap, re-pinned to their pre-gap integer instead of re-leveling from 0
    int eq34_mismatch_epochs = 0;
    // The pair family (network.phase_datum_eq34_family "pair"); all four stay 0 with the knob off.
    // A pair family that is elected but never attaches a band-only vertex is the ordinary case on a
    // bed where every arc carries both bands, so the last two are read against the bed, not alone.
    long long eq34_pair_families = 0;         ///< pair families elected: constellation-epochs with exactly two phase signals of different bands
    long long eq34_pair_fallbacks = 0;        ///< constellation-epochs that formed no pair family and were elected per signal (one signal, three or more, or two of one band)
    long long eq34_pair_ext_edges = 0;        ///< tree edges the band extension added: a band-only vertex attached to a pair tree, on its own band
    long long eq34_pair_band_only_comps = 0;  ///< band-only components elected on their own band: a band component with no vertex in any pair tree
    // The kept row of the chain's event table (network.phase_datum_eq34_event_rows "kept"); all stay 0
    // with the row off.  An incumbent arc is a pinned tree edge whose arc was a tree edge at the last
    // election, in either direction.  The row holds it (previous target kept, no frame step of its own)
    // or not; every incumbent arc of an election lands in exactly one of the tallies below, the first
    // cause that applies in the order listed.  The tallies are per election: an edge counts once per
    // election, and a target the as-built rule would re-read differently counts at every election
    // until the two agree (the observatory counts such a difference once, at its onset).  The
    // seed-round tallies above count the as-built draft of a held edge as well.
    long long eq34_kept_held = 0;             ///< incumbent arcs the row held
    long long eq34_kept_flipped = 0;          ///< ...of them, arcs whose direction reversed (the as-built rule read a re-host)
    long long eq34_kept_unknown = 0;          ///< ...of them, edges whose as-built step was unknown (a re-reference of the subtree)
    long long eq34_kept_step_whole = 0;       ///< ...of them, edges whose as-built step was a known nonzero step of whole cycles
    long long eq34_kept_step_frac = 0;        ///< ...of them, edges whose as-built step was a known nonzero step that is not whole cycles (or that has no wavelength)
    long long eq34_kept_rehost = 0;           ///< ...of them, edges whose as-built rule read a host change
    long long eq34_kept_not_freed = 0;        ///< incumbent arcs not held: the arc is in the epoch's free-epoch latch (the slip row's case)
    long long eq34_kept_not_root = 0;         ///< ...a satellite's remembered root differs from the component's datum receiver (the root row's case)
    long long eq34_kept_not_target = 0;       ///< ...the arc has no previous target
    long long eq34_kept_not_life = 0;         ///< ...the arc has no remembered pin life (not pinned at the last election, or a warm resume)
    long long eq34_kept_not_ended = 0;        ///< ...the arc's pinned life ended (reborn, re-entered after a gap, flagged)
    // The returning row of the chain's event table (network.phase_datum_eq34_event_rows "returning"); all
    // stay 0 with the row off.  A returning edge is a tree edge whose child vertex was absent from the last
    // election's tree on its band and a vertex of an earlier one.  The row releases it (the cut release) or,
    // when its arc is in the epoch's free-epoch latch, leaves it to the slip row.  The as-built tallies state
    // what the as-built draft of a released edge had been, before the release replaced it; they and the
    // released count are per election.  The cut tallies are what the releases took, per release.
    long long eq34_ret_released = 0;          ///< returning edges the row released
    long long eq34_ret_not_freed = 0;         ///< returning edges not released: the arc is in the epoch's free-epoch latch (the slip row's case)
    long long eq34_ret_with_state = 0;        ///< ...released edges whose moved side held at least one leveling carry, receiver phase-bias carry or satellite phase-bias prior that the release took
    long long eq34_ret_asbuilt_zero = 0;      ///< ...released edges whose as-built step was a known zero
    long long eq34_ret_asbuilt_whole = 0;     ///< ...a known nonzero step of whole cycles
    long long eq34_ret_asbuilt_frac = 0;      ///< ...a known nonzero step that is not whole cycles (or that has no wavelength)
    long long eq34_ret_asbuilt_unknown = 0;   ///< ...an unknown step
    long long eq34_ret_asbuilt_pinround = 0;  ///< ...released edges whose as-built target was read from the arc's old-frame carry (pin-to-round)
    long long eq34_ret_arcs = 0;              ///< leveling carries the releases erased: every carry of a satellite of a moved side at any station, and every carry of a receiver of a moved side on its band
    long long eq34_ret_rpb = 0;               ///< receiver phase-bias carries the releases dropped
    long long eq34_ret_pb = 0;                ///< satellite phase-bias priors of the moved sides (measure-only: the re-reference marginalizes them)
    long long eq34_ret_declared = 0;          ///< (satellite, signal) keys of the moved sides, declared moved
    // The rehost row of the chain's event table (network.phase_datum_eq34_event_rows "rehost"); all stay 0
    // with the row off.  A re-host edge is a tree edge whose child had a parent at the last election and
    // whose arc was no tree edge then, in either direction.  The row releases it (the cut release the
    // returning row takes) or, when its arc is in the epoch's free-epoch latch, leaves it to the slip row.
    // The as-built tallies state what the as-built draft of a released edge had been, before the release
    // replaced its step; they and the released count are per election.  The cut tallies are what the
    // releases took, per release.
    long long eq34_rh_released = 0;           ///< re-host edges the row released
    long long eq34_rh_not_freed = 0;          ///< re-host edges not released: the arc is in the epoch's free-epoch latch (the slip row's case)
    long long eq34_rh_with_state = 0;         ///< ...released edges whose moved side held at least one leveling carry, receiver phase-bias carry or satellite phase-bias prior that the release took
    long long eq34_rh_carry = 0;              ///< ...released edges whose as-built target was read from the new host's carried value (pin-to-round)
    long long eq34_rh_cold = 0;               ///< ...released edges with no carried value at the new host
    long long eq34_rh_asbuilt_zero = 0;       ///< ...released edges whose as-built step was a known zero
    long long eq34_rh_asbuilt_whole = 0;      ///< ...a known nonzero step of whole cycles
    long long eq34_rh_asbuilt_frac = 0;       ///< ...a known nonzero step that is not whole cycles (or that has no wavelength)
    long long eq34_rh_asbuilt_unknown = 0;    ///< ...an unknown step
    long long eq34_rh_arcs = 0;               ///< leveling carries the rehost releases erased: every carry of a satellite of a moved side at any station, and every carry of a receiver of a moved side on its band
    long long eq34_rh_rpb = 0;                ///< receiver phase-bias carries the rehost releases dropped
    long long eq34_rh_pb = 0;                 ///< satellite phase-bias priors of the moved sides (measure-only: the re-reference marginalizes them)
    long long eq34_rh_declared = 0;           ///< (satellite, signal) keys of the moved sides, declared moved
    // The root row of the chain's event table (network.phase_datum_eq34_event_rows "root"); all stay 0
    // with the row off.  A handover is a component of one band whose datum receiver is not the root its
    // satellites remember.  The row pins the new root's receiver phase bias at its carried value when the
    // new root was a vertex of the last tree, keeps the old root's pieces apart (only the piece that keeps
    // the old root is pinned) and finds a carried bias inside the horizon; every other handover runs the
    // as-built rule.  The tallies are per election, one per component and band.
    long long eq34_root_handovers = 0;        ///< handovers seen: components whose root is new and whose satellites remember another root
    long long eq34_root_pinned = 0;           ///< ...handovers whose new root is pinned at its carried receiver phase bias
    long long eq34_root_kept_edges = 0;       ///< satellite edges of pinned handovers the as-built rule would have marked unknown and whose frame stayed in the old root's (not released, and not below a cut release or another unknown step)
    long long eq34_root_not_vertex = 0;       ///< ...handovers left as built: the new root was no vertex of the last tree on the band
    long long eq34_root_not_keeper = 0;       ///< ...handovers left as built: the component is a split piece other than the one that keeps the old root
    long long eq34_root_not_carry = 0;        ///< ...handovers left as built: no receiver phase-bias carry for the new root inside the horizon
    // The slip row of the chain's event table (network.phase_datum_eq34_event_rows "slip"); all stay 0 with
    // the row off.  An arc in the epoch's free-epoch latch leaves the tree while another route reaches its
    // child.  A de-seated edge is an incumbent tree edge of a band that left the band's tree this way; the new
    // edge that re-attaches its child is that edge's own row's, and what a release took there is counted under
    // that row.  An edge on a latched arc that no other route could replace is released here, on the band whose
    // arc is latched.  The tallies are per election and band; the cut tallies are what the slip row's releases took.
    long long eq34_sl_deseated = 0;           ///< incumbent tree edges de-seated: the arc is latched on some band of the family and is no tree edge of this band now
    long long eq34_sl_deseated_family = 0;    ///< ...of them, edges whose arc is latched only on the family's other band
    long long eq34_sl_released = 0;           ///< edges on a latched arc the row released: no other route reached the child
    std::array<long long, 8> eq34_sl_by_cause{};   ///< de-seated and released edges by the latch's cause bits on their band (RelevelEpochState::kCause*, bit order); an edge counts under each of its bits
    long long eq34_sl_birth = 0;              ///< edges on a latched arc left as built: the child was never a vertex of a tree
    long long eq34_sl_with_state = 0;         ///< ...released edges whose moved side held at least one leveling carry, receiver phase-bias carry or satellite phase-bias prior that the release took
    long long eq34_sl_asbuilt_zero = 0;       ///< ...released edges whose as-built step was a known zero
    long long eq34_sl_asbuilt_whole = 0;      ///< ...a known nonzero step of whole cycles
    long long eq34_sl_asbuilt_frac = 0;       ///< ...a known nonzero step that is not whole cycles (or that has no wavelength)
    long long eq34_sl_asbuilt_unknown = 0;    ///< ...an unknown step
    long long eq34_sl_arcs = 0;               ///< leveling carries the slip row's releases erased: every carry of a satellite of a moved side at any station, and every carry of a receiver of a moved side on its band
    long long eq34_sl_rpb = 0;                ///< receiver phase-bias carries the slip row's releases dropped
    long long eq34_sl_pb = 0;                 ///< satellite phase-bias priors of the moved sides (measure-only: the re-reference marginalizes them)
    // The merge and split row (network.phase_datum_eq34_event_rows "merge_split"; all 0 with the row off).
    long long eq34_split_pinned = 0;          ///< split pieces (without the old root) whose new root is pinned at its carried receiver phase bias, per band
    long long eq34_split_no_carry = 0;        ///< ...split pieces left as built: no receiver phase-bias carry for the new root inside the horizon
    long long eq34_mg_released = 0;           ///< tree edges released because they join two trees of the last election
    long long eq34_mg_absorbed_held = 0;      ///< tree edges inside an absorbed side that the kept row held at their targets
    long long eq34_mg_with_state = 0;         ///< ...released edges whose moved side held at least one leveling carry, receiver phase-bias carry or satellite phase-bias prior
    long long eq34_mg_asbuilt_zero = 0;       ///< ...released edges whose as-built step was a known zero
    long long eq34_mg_asbuilt_whole = 0;      ///< ...a known nonzero step of whole cycles
    long long eq34_mg_asbuilt_frac = 0;       ///< ...a known nonzero step that is not whole cycles (or that has no wavelength)
    long long eq34_mg_asbuilt_unknown = 0;    ///< ...an unknown step
    long long eq34_mg_arcs = 0;               ///< leveling carries the merges' releases erased
    long long eq34_mg_rpb = 0;                ///< receiver phase-bias carries the merges' releases dropped
    long long eq34_mg_pb = 0;                 ///< satellite phase-bias priors of the moved sides (measure-only: the re-reference marginalizes them this epoch)
    long long eq34_mg_declared = 0;           ///< (satellite, signal) keys of the moved sides, declared moved
    long long eq34_sl_declared = 0;           ///< (satellite, signal) keys of the moved sides, declared moved
};

} // namespace GnssProcessor
