#pragma once
/**
 * @file NetworkChainDatum.hpp
 * @brief Eq-34 / de Jonge single-receiver-chain phase-bias datum for the PPP-RTK
 *        network producer (Card 3).
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
 * the candidate census consumed here, the two non-chain realizations (ensemble
 * datum, anchor election) and the drop-and-heal.  Owns no state: every carry object
 * is a reference into the caller's NetworkRunContext.
 *
 * Ordering contract: this function mutates persistent carries before the solve.  It
 * erases local_carry entries for re-referenced (sat,σ), erases and shifts rpb_carry,
 * shifts prev_carry PhaseBias entries, and fills ctx.carry.eq34_reref (read
 * downstream by networkInjectD2CarryPrior, networkStashCarry and the M2 VCE
 * shared-prior snapshot).  It therefore runs:
 *   - after networkAssemblyPrefilter_ (its phase-bias prune is a precondition);
 *   - before the Card-3 D3c RPB allocation, which stays in the run TU.  This
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

#include "filter/NetworkRunTypes.hpp"   // NetStation / PhaseDatumCandidates
#include "filter/NetworkAnchorHygiene.hpp"   // AnchorHygiene (eq34 reattach hygiene)
#include "filter/NetworkCarrySites.hpp"      // CarrySiteCounts (card PRM phase 2)

#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class LogManager;
struct NetworkRunContext;   // app/NetworkRunContext.hpp, included in the .cpp only

/**
 * @brief The eq34 chain datum's D4 carry shift through the traversal of carried references
 *        (card PRM phase 2, @c CarryMechanism::Eq34Shift).
 *
 * The rule of the D4 apply: a carried ambiguity moves by
 * @f$-\Delta b_s - \Delta\delta_r@f$ (its satellite's and its receiver's chain steps, formed in
 * that order and added once), a carried receiver phase bias by its own
 * @f$\Delta\delta_r@f$, a carried satellite phase bias by @f$\Delta b_s@f$, and the s_prev of
 * every phase-bias coupling term on a shifted satellite column (in the ambiguity, receiver
 * phase-bias and wet-delay carries) by @f$\Delta b_s@f$.  A non-finite or zero step moves
 * nothing.
 * @param carry     the run's carried state.
 * @param shift_sat (satellite, signal) -> @f$\Delta b_s@f$.
 * @param shift_rxg (station, constellation, signal) -> @f$\Delta\delta_r@f$.
 * @param shift_rpb (station, receiver phase-bias key) -> @f$\Delta\delta_r@f$.
 * @param dry       count only (nothing is written).
 * @param n_values  out: carried values moved (ambiguities, receiver and satellite phase biases).
 * @param n_terms   out: coupling terms moved.
 * @param counts    optional out: per-site tallies (accumulated).
 */
void eq34ShiftCarry(NetworkCarryState& carry,
                    const std::map<std::pair<std::string, std::string>, double>& shift_sat,
                    const std::map<std::tuple<int, char, std::string>, double>& shift_rxg,
                    const std::map<std::pair<int, std::string>, double>& shift_rpb,
                    bool dry, long long& n_values, long long& n_terms,
                    CarrySiteCounts* counts = nullptr);

/**
 * @brief Elect the Card-3 eq34 chain datum for this epoch.
 *
 * Resets every station's pins, seeds the shared set from the (post-gate) union,
 * builds the per-(constellation,signal) bipartite graph, elects r1 and grows a
 * sticky spanning tree per connected component, then runs the carry hygiene
 * (re-reference erase + S-transformation apply + I2 extend-only merge-back) and
 * advances the sticky baselines.
 *
 * @param ctx           [in/out] carry + counters.  The whole context is passed
 *                      instead of about twenty individual references because the
 *                      body value-mutates local_carry / rpb_carry / prev_carry, all
 *                      of which are copyable: a single missing `&` in an enumerated
 *                      parameter list would compile and silently drop the
 *                      S-transform.  The body opens with an alias preamble that
 *                      reproduces the former run-TU local names.
 * @param config_       networkPhaseDatumEq34Receiver / Eq34PinRound.
 * @param log_manager_  The only Application dependency: lazy construction of
 *                      ctx.carry.eq34_ppp_utils.  Keep the construction here rather
 *                      than in the caller: PppUtils' constructor logs, so hoisting it
 *                      changes spdlog output on every non-eq34 arm, and the pointer
 *                      is read again at the emit-rebase site later in the epoch.
 * @param log           "main_log": the pin/gauge-mismatch warning + 2 debug lines.
 * @param stations      [in/out] pinned_amb_targets / pinned_rpb_keys.
 * @param present       This epoch's presence mask.
 * @param cnd           Candidate census (sat_el / usable_keys / cover / union_keys).
 * @param hyg           eq34 reattach hygiene verdicts for this epoch
 *                      (filter/NetworkAnchorHygiene.hpp).  Default-constructed
 *                      (inactive), the election is unchanged from the pre-hygiene
 *                      behavior: every arc is an eligible host, nothing is bridged
 *                      and no pin is σ-scaled.  When active it (a) restricts which
 *                      arcs may become new tree edges, (b) adds spanning-forest
 *                      roots (a bridged satellite is grounded by its shared
 *                      pseudo-obs row instead of a parent edge) and (c) supplies the
 *                      INC-3 σ multiplier for pins the eligible pass could not avoid.
 * @param shared_phase_keys [out] the post-gate union's phase-bias keys.
 */
void networkElectChainDatum(NetworkRunContext& ctx,
                            const GnssConfig& config_,
                            LogManager& log_manager_,
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
                            std::set<std::string>& shared_phase_keys);

} // namespace GnssProcessor
