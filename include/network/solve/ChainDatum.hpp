#pragma once
/**
 * @file ChainDatum.hpp
 * @brief Eq-34 / de Jonge single-receiver-chain phase-bias datum for the PPP-RTK
 *        network producer.
 *
 * For each (constellation, signal) connected component of the bipartite
 * (receiver × satellite) observation graph, the election picks a datum receiver r1
 * (its receiver phase bias is pinned to 0) and pins the ambiguities on the edges of a
 * sticky spanning tree rooted at r1.  Every between-satellite reference is then an
 * integer combination across the whole component, including satellites r1 does not
 * see, because each chain hop contributes only an integer: the hop receiver's
 * fractional bias is its own estimable RPB state δ_{r,σ} and is not part of the pin.
 * With full visibility (the regional case) the chain reduces to the Odijk Eq-34 star
 * (CC-R).
 *
 * Gauge bookkeeping, cross-checked every epoch (the rank invariant D3c rests on it):
 * a component with n_c stations and m_c satellites gets (n_c + m_c − 1) tree-edge
 * ambiguity pins plus 1 RPB pin, n_c + m_c in total, which equals the gauge
 * dimension.  Every RPB-carrying station is a tree vertex, so it gets at least one
 * pin and its local normal block N_rr stays positive-definite.
 *
 * Split out of Application::runNetwork at decomposition INC-8b (see
 * docs/SPEC_inc8b_election_split.md).  Sibling of @ref NetworkPhaseDatum, which owns
 * the candidate census consumed here, the non-chain realization (the anchor
 * election) and the drop-and-heal.  Owns no state: every carry object
 * is a reference into the caller's NetworkRunContext.
 *
 * Ordering contract: this function mutates persistent carries before the solve.  It
 * erases local_carry entries for re-referenced (sat,σ), erases and shifts rpb_carry,
 * shifts prev_carry PhaseBias entries, and fills ctx.carry.eq34_reref (read
 * downstream by networkInjectD2CarryPrior, networkStashCarry and the M2 VCE
 * shared-prior snapshot).  It therefore runs:
 *   - after networkAssemblyPrefilter_ (its phase-bias prune is a precondition);
 *   - before the D3c RPB allocation, which stays in the run TU.  This
 *     function is the only writer of shared_phase_keys on the eq34 path, and D3c
 *     derives this epoch's need_rpb set from it.  If D3c ran first it would allocate
 *     and prune RPB states against the previous epoch's shared set: this epoch's
 *     pinned_rpb_keys could name just-pruned states, and stations newly entering the
 *     D2 graph would get no δ_{r,σ} column.  The rank cross-check counts recorded
 *     pins, not allocated columns, so it would not report this; the defect would show
 *     only as null-space-gate withholds and non-PD local blocks;
 *   - and hence before est.reset() / est.beginEpoch() (inside
 *     networkAssemblyRegisterColumns_), networkPassB_ and networkStashCarry.
 * Inside the function, the I2 extend-only merge runs before the sticky baseline
 * advance `anchor_incumbent = this_epoch_anchor;`.
 */

#include "network/driver/RunTypes.hpp"   // NetStation / PhaseDatumCandidates
#include "network/solve/AnchorHygiene.hpp"   // AnchorHygiene (eq34 reattach hygiene)
#include "network/state/CarrySites.hpp"      // CarrySiteCounts

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
 * @brief The eq34 chain datum's D4 carry shift through the traversal of carried references
 *        (@c CarryMechanism::Eq34Shift).
 *
 * The rule of the D4 apply: a carried ambiguity moves by
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
 * @param complete  also move the owed sites (false = the original walk, without them).
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
 * builds the per-(constellation,signal) bipartite graph, elects r1 and grows a
 * sticky spanning tree per connected component, then runs the carry hygiene
 * (re-reference erase + S-transformation apply + I2 extend-only merge-back) and
 * advances the sticky baselines.
 *
 * The graph, its components and the tree are those of filter/NetworkArcGraph:
 * arcgraph::buildArcGraph per group (stations in roster order,
 * satellites in string order, arcs in usable_keys order), arcgraph::arcComponents in
 * the legacy order (arcgraph::legacyChainVertexRank) and arcgraph::growChainForest per
 * component (roots r1 and the bridged satellites, the live incumbent arcs as the
 * sticky seed).  The pins are recorded by replaying the tree's edges in attachment
 * order after the walk, which is exact because the walk reads nothing a pin writes.
 *
 * @param ctx           [in/out] carry + counters.  The whole context is passed
 *                      instead of about twenty individual references because the
 *                      body value-mutates local_carry / rpb_carry / prev_carry, all
 *                      of which are copyable: a single missing `&` in an enumerated
 *                      parameter list would compile and silently drop the
 *                      S-transform.  The body opens with an alias preamble that
 *                      reproduces the former run-TU local names.
 * @param config_       networkPhaseDatumEq34Receiver / Eq34PinRound.
 * @param log           "main_log": the pin/gauge-mismatch warning + 2 debug lines.
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
 *                      INC-3 σ multiplier for pins the eligible pass could not avoid.
 * @param shared_phase_keys [out] the post-gate union's phase-bias keys.
 * @param events The epoch's events; writes phase_datum.
 */
void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            std::vector<NetStation>& stations,
                            const std::vector<bool>& present,
                            const PhaseDatumCandidates& cnd,
                            const AnchorHygiene& hyg,
                            /// This epoch's complete C3c slip funnel as (station index,
                            /// ambiguity key).  Read only under
                            /// network.phase_datum_eq34_slip_aware_election, to keep a slipped
                            /// arc out of the spanning tree where the graph allows it (a
                            /// slipped tree edge refloats every fundamental cycle through it; a
                            /// slipped chord costs one DD).  The caller must not insert into
                            /// the funnel after this call (see the ordering contract above);
                            /// the election already runs after every PASS-A drain, signal
                            /// integrity and measurement admission.
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys, EpochEvents& events);

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the eq34 chain datum: re-references, pin audit, seed round, slip-aware election.
 *
 * The eq34 chain datum: the re-reference totals, the pin audit, the seed round
 * and the slip-aware election.
 *
 * Gate: each section under its own condition, as in the teardown:
 * `config_.networkPhaseDatumEq34()`; `config_.networkPhaseDatumEq34SeedRound()`;
 * `config_.networkPhaseDatumEq34SlipAwareElection()`.
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

// ---- PASS B serial stages, declarations (generated by scripts/nom_pb1_cut.py) ----

/**
 * @brief The eq34 chain-pin audit: declared, folded, landed and short pins per group, and the
 *        seed-round targets of accepted stations for the next election.
 *
 * No-op unless NetworkKnobs::phase_datum_eq34 and the epoch was not aborted. Called by
 * networkPassB_ after the consume.
 *
 * Epoch form (generated by scripts/nom_pb1_cut.py): a serial stage of
 * Application::networkPassB_, cut with the alias lines of networkPassB_ it reads.  Below its
 * bindings is the frozen text; a piece wholly inside one `if` returns early instead
 * (scripts/nom_pb1/declared.py).
 *
 * @param run The run (network/driver/Run.hpp). Bindings: run.ctx (ctx), run.setup (setup).
 * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: none.
 * @param assembly The station blocks, read (pin_audit, pin_seed_targets, est_index_of), passed
 *                 because networkPassB_ still writes the group. Bindings: assembly.asm_res
 *                 (asm_res), assembly.est_index_of (est_index_of).
 * @param epoch_ok The consume's verdict (kept).
 */
void networkEq34PinAudit(NetworkRun& run, const NetworkEpoch& ep, const EpochAssembly& assembly,
                         bool epoch_ok);

// ---- End of the PASS B serial stages, declarations ----

// ---- Harvest half, declarations (generated by scripts/nom_hv1_cut.py) ----

struct NetworkRun;                          // network/driver/Run.hpp
struct NetworkEpoch;                        // network/driver/Epoch.hpp
struct HarvestBorder;                       // network/carry/LocalCarryHarvest.hpp
struct StationHarvestBlock;                 // network/carry/LocalCarryHarvest.hpp

/**
 * @brief Eq34: each receiver phase bias with its diagonal conditional variance (no
 *        coupling, by design), then the prune. No-op unless network.eq34_rpb_q > 0.
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
    //                 arc counts once per epoch (it counted the
    //                 sort's comparator calls before);
    //   * readmitted: slipped arcs that became tree edges anyway, because the
    //                 component had no other route to the vertex.  Those still
    //                 mass-refloat, and a large share here means the topology, not the
    //                 election, is the binding constraint (standing invariant #4:
    //                 report the failure mode, and do not let "deseeded > 0" read as
    //                 "no tree edge slipped").
    long long eq34_slip_deseeded   = 0;
    long long eq34_slip_demoted    = 0;
    long long eq34_slip_readmitted = 0;
    // Run tallies of the chain-pin audit (docs/SPEC_synthetic_minimum_network.md
    // §14 T1; measured record §12.8), keyed "<cons>|<signal>" (the election's dr_key).
    // A pin can leak silently from a three-stage pipeline: declared (the election's
    // per-group Σ (n_c + m_c) gauge dimension), folded (foldAmb/foldRpb built the 1e10
    // row; the lambdas return early on a missing state index or a missing live phase
    // row), and landed (the folded row sits on a station that est.addStation accepted;
    // a rejected or dropped station takes every folded pin down with it, the loss mode
    // network.station_pd_pin_aware exists to prevent).  A group whose landed <
    // declared − bridged in any epoch ran with an under-constrained datum that reads
    // like a working one (standing invariant #4); the W6+SH n-invariant L2W near-null
    // is the suspect that motivated the audit.  Accumulated serially after PASS B's
    // consume; logged once by networkChainDatumRunEnd.  All zero unless
    // network.phase_datum_eq34.
    struct Eq34PinAudit {
        std::map<std::string, long long> declared;         ///< Σ per-epoch gauge dim (n_c+m_c)
        std::map<std::string, long long> bridged;          ///< Σ hygiene INC-2 bridge pins (shared-side)
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
    long long seed_round_frozen       = 0;   ///< targets reused verbatim from eq34_prev_target, so the frame is realized once (INC-1)
    long long seed_round_target_steps = 0;
    double    seed_round_max_abs_y_m  = 0.0;
    // Re-host tallies (INC-2).  `framed` = re-hosts whose frame step was computed
    // from the new host's own state and therefore stayed shiftable (the carried PB
    // prior is S-transformed, not marginalized); `cold` = re-hosts that INC-2 could
    // not frame (no allocated state: the fold-deferred path), which keep the legacy
    // unknown-step reref.  `max_step_m` is the largest |frame step| any framed re-host
    // applied.  It is bounded by λ/2 only when the new host's float is
    // frame-consistent, so a large value signals that the solution at the new host had
    // drifted off the realized frame: read it as a diagnostic of the runaway the audit
    // probes, never as a quality bound.
    long long seed_round_rehost_framed  = 0;
    long long seed_round_rehost_cold    = 0;
    double    seed_round_rehost_max_step_m = 0.0;

    // (D) One-solve frame skips: the frame stepped, so the carried value is stale in the
    //     old gauge; the same epoch's harvest re-stashes any still-tracked arc, so the
    //     cost is one solve of continuity.
    long long n_cr_eq34_reref         = 0;  ///< eq34 D4 chain-datum re-reference
    long long n_cr_rpb_gap_prune      = 0;  ///< receiver-PB carry entries dropped by the pl_max_gap_s prune
    long long n_resume_datum_first_reref    = 0; ///< PHASE_DATUM/eq34_reref (sat,σ) at the first post-resume epoch
    // Held phase-bias emissions (pb_hold) of a (satellite,
    // signal) family that a phase-bias shift moved (the eq34 D4 shift, the declared anchor
    // handover) and that stayed in the old frame: PbHold is exempt from both shifts with or
    // without network.carry_rebase_complete (its own design is owed).  Measure-only.
    long long prm_pbhold_unmoved_at_shift = 0;
    // Run-scope observability counters of the eq34 datum, logged once in the run-end producer
    // summary (the per-epoch reref/erase lines are debug-only, so these totals are the
    // only non-debug record of the reref volume).  Untouched on non-eq34 runs.
    long long eq34_reref_total = 0, eq34_erase_total = 0;
    long long eq34_shift_total = 0;   // carry values S-transformed (v2 shift) over the run
    long long eq34_cpl_sprev_total = 0;   // coupling s_prev references re-based with those shifts; structurally 0 with coupling off, since the stashes are empty (SPEC_card1_coupling_carry §14)
    long long eq34_gap_repin_total = 0;   // tree edges resumed from a carry-alive gap, re-pinned to their pre-gap integer instead of re-leveling from 0 (I2)
    int eq34_mismatch_epochs = 0;
};

} // namespace GnssProcessor
