#pragma once
/**
 * @file NetworkChainDatum.hpp
 * @brief Card-3 Eq-34 / de Jonge SINGLE-RECEIVER-CHAIN phase-bias datum for the
 *        PPP-RTK network producer.
 *
 * Per (constellation,signal) connected component of the bipartite
 * (receiver × satellite) observation graph: elect a datum receiver r1 (its
 * receiver phase-bias is pinned to 0) and pin the ambiguities on the edges of a
 * STICKY spanning tree rooted at r1.  Then every between-satellite reference is
 * an integer combination across the WHOLE component — including satellites r1
 * never sees — because each chain hop contributes only an integer (the hop
 * receiver's fractional bias is its OWN estimable RPB state δ_{r,σ}, no longer
 * part of the pin).  The full-visibility regional case degenerates to the Odijk
 * Eq-34 star (CC-R).
 *
 * Gauge bookkeeping (cross-checked every epoch, and the reason the rank
 * invariant D3c holds): a component with n_c stations and m_c satellites gets
 * (n_c + m_c − 1) tree-edge ambiguity pins + 1 RPB pin = n_c + m_c, exactly the
 * gauge dimension.  Every RPB-carrying station is a tree vertex, so it gets ≥ 1
 * pin ⇒ its local normal block N_rr stays positive-definite.
 *
 * Seeded at decomposition INC-8b (2026-07-22), which relocated the eq34
 * election VERBATIM out of Application::runNetwork (see
 * docs/SPEC_inc8b_election_split.md).  Sibling of @ref NetworkPhaseDatum, which
 * owns the candidate census this consumes plus the two non-chain realizations
 * (ensemble datum, anchor election) and the drop-and-heal.  Owns NO state:
 * every carry object is an explicit reference into the caller's
 * NetworkRunContext.
 *
 * ⚠ ORDERING CONTRACT — this MUTATES persistent carries PRE-SOLVE.  It erases
 * local_carry entries for re-referenced (sat,σ), erases/shifts rpb_carry,
 * shifts prev_carry PhaseBias entries, and fills ctx.carry.eq34_reref (read
 * downstream by networkInjectD2CarryPrior, networkStashCarry and the M2 VCE
 * shared-prior snapshot).  Therefore it MUST run:
 *   - AFTER  networkAssemblyPrefilter_ (its phase-bias prune is a precondition);
 *   - BEFORE the Card-3 D3c RPB allocation, which stays in the run TU — this
 *     function is the SOLE writer of shared_phase_keys on the eq34 path and
 *     D3c derives this epoch's need_rpb set from it.  Running D3c first would
 *     allocate/prune RPB against the PREVIOUS epoch's shared set, so this
 *     epoch's pinned_rpb_keys could name just-pruned states while stations
 *     newly entering the D2 graph get no δ_{r,σ} column at all.  The epoch's
 *     own rank cross-check counts RECORDED pins, not allocated columns, so it
 *     would stay silent and the defect would surface only as null-space-gate
 *     withholds / non-PD local blocks on field data;
 *   - and hence BEFORE est.reset() / est.beginEpoch() (inside
 *     networkAssemblyRegisterColumns_), networkPassB_ and networkStashCarry.
 * Internally, the I2 extend-only merge must stay BEFORE the sticky baseline
 * advance `anchor_incumbent = this_epoch_anchor;`.
 */

#include "filter/NetworkRunTypes.hpp"   // NetStation / PhaseDatumCandidates
#include "filter/NetworkAnchorHygiene.hpp"   // AnchorHygiene (eq34 reattach hygiene)

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class LogManager;
struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only

/**
 * @brief Elect the Card-3 eq34 chain datum for this epoch.
 *
 * Resets every station's pins, seeds the shared set from the (post-gate) union,
 * builds the per-(constellation,signal) bipartite graph, elects r1 and grows a
 * sticky spanning tree per connected component, then runs the carry hygiene
 * (re-reference erase + S-transformation apply + I2 extend-only merge-back) and
 * advances the sticky baselines.
 *
 * @param ctx           [in/out] carry + counters.  Taken as the whole context
 *                      rather than ~22 individual references on purpose: the
 *                      body value-mutates local_carry / rpb_carry / prev_carry,
 *                      all of which are copyable, so a single dropped `&` in an
 *                      enumerated parameter list would compile clean and
 *                      silently delete the S-transform.  The body opens with an
 *                      alias preamble that is a byte-copy of the former run-TU
 *                      alias block, so the relocated code reads the old names.
 * @param config_       networkPhaseDatumEq34Receiver / Eq34PinRound.
 * @param log_manager_  Sole Application dependency: the LAZY construction of
 *                      ctx.carry.eq34_ppp_utils.  Do NOT hoist it to the caller
 *                      — PppUtils' constructor logs (hoisting changes spdlog
 *                      output on every non-eq34 arm) and the pointer is re-read
 *                      at the emit-rebase site much later in the epoch.
 * @param log           "main_log": the pin/gauge-mismatch warning + 2 debug lines.
 * @param stations      [in/out] pinned_amb_targets / pinned_rpb_keys.
 * @param present       This epoch's presence mask.
 * @param cnd           Candidate census (sat_el / usable_keys / cover / union_keys).
 * @param hyg           eq34 REATTACH HYGIENE verdicts for this epoch
 *                      (filter/NetworkAnchorHygiene.hpp).  Default-constructed
 *                      (inactive) reproduces the pre-card election exactly: every
 *                      arc is an eligible host, nothing is bridged and no pin is
 *                      σ-scaled.  When active it (a) restricts which arcs may become
 *                      NEW tree edges, (b) contributes extra spanning-FOREST ROOTS —
 *                      a bridged satellite is grounded by its shared pseudo-obs row
 *                      instead of a parent edge — and (c) supplies the INC-3 σ
 *                      multiplier for pins the eligible pass could not avoid.
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
                            /// This epoch's complete C3c slip funnel — (station index, ambiguity
                            /// key).  Read ONLY under network.phase_datum_eq34_slip_aware_election,
                            /// to keep a slipped arc out of the spanning tree where the graph
                            /// allows it (a slipped TREE edge refloats every fundamental cycle
                            /// through it; a slipped CHORD costs one DD).  The caller must not
                            /// insert into the funnel after this point — see the ordering
                            /// contract above; the election already sits after every PASS-A
                            /// drain, signal integrity and measurement admission.
                            const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                            std::set<std::string>& shared_phase_keys);

} // namespace GnssProcessor
