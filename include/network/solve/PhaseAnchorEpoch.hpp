#pragma once
#include <cstddef>
/**
 * @file PhaseAnchorEpoch.hpp
 * @brief The phase datum of one network epoch: the shared phase-bias set, the station that
 *        anchors each (sat,signal), and the eq34 receiver phase-bias allocation
 *        (a stage of the per-epoch spine).
 *
 * The bodies are the frozen driver text (main 7141240), moved by scripts/nom_ep2_cut.py and
 * checked by scripts/nom_ep2_verify.py.  The algorithms they call are elsewhere: the candidate
 * census and the elections in filter/NetworkPhaseDatum, the reattach hygiene in
 * filter/NetworkAnchorHygiene, the chain datum in filter/NetworkChainDatum.
 *
 * Position (Application::networkProcessEpoch_): after the PASS-A slip drains and the two
 * admission screens (the funnel is complete, and their withholds have pruned the states an
 * election could otherwise pin), and before the level-authority census, the column
 * registration, est.beginEpoch and PASS B.
 */

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct EpochEvents;             // network/driver/Epoch.hpp
struct EpochPassA;

/**
 * @brief The final shared phase-bias set and the anchor election.
 *
 * Legacy (network.phase_datum_prioritized off): the pivot's phase-bias states are the shared
 * set.  Prioritized: the candidate census and coverage gate (networkBuildPhaseDatumCandidates),
 * the eq34 reattach hygiene, then either the chain datum (network.phase_datum_eq34) or the
 * two-edge continuous handover election (anchor-slip handover, cold fallback candidacy, the
 * held S-basis, the incumbent swap and its carry re-reference).  Writes events (PHASE_ANCHOR,
 * INJECT, PBGAUGE, STRANSFORM) and the phase-datum counters.
 *
 * @param run    The run: the stations' phase-bias states, ctx.carry (anchors, incumbents, the
 *               re-referenced set, the held basis), ctx.counters, the traces.
 * @param ep     The epoch, read: se, funnel.carry_slipped_ambs.
 * @param pass_a PASS A's layout: reads modeled and present, writes shared_phase_keys.
 * @param events The epoch's events; written: phase_datum.
 */
void networkPhaseAnchorEpoch(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a, EpochEvents& events);

/**
 * @brief The eq34 receiver phase-bias allocation (rule D3c).
 *
 * A present station carries the per-(receiver,system,signal) split state if and only if it
 * carries an ungated shared phase-bias column on that signal; the set is re-derived each epoch
 * (allocate what is needed, prune the rest), before est.reset() and PASS B.  Inert (no
 * allocation) unless network.phase_datum_eq34.
 *
 * @param run The run: the stations' parameter sets (written).
 * @param ep  The epoch, read: pass_a.present, pass_a.shared_phase_keys.
 */
void networkReceiverPhaseBiasAllocation(NetworkRun& run, const NetworkEpoch& ep);

/// @brief Run-scope counters of PhaseAnchorEpoch, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct PhaseAnchorEpochCounters {
    // On-path telemetry of the held S-basis (RF-2; invariant #4).
    long long n_pb_basis_held          = 0;  ///< family-epochs re-pinned at their frozen basis value (the held property in action).
    long long n_pb_basis_elected       = 0;  ///< new basis entries written: first sightings plus handovers.
    long long n_pb_basis_cold          = 0;  ///< …of which cold (no level to inherit): the new basis is 0 and the emitted phase bias steps by the outgoing value.
    long long n_pb_basis_refuse_young  = 0;  ///< candidate-family evaluations refused by the arc-age gate (never a newborn low arc).
    long long n_pb_basis_refuse_flagged= 0;  ///< …refused by the screen-flag quiet window (item 3), which is also item 4's depose trigger on the incumbent.
    long long n_pb_basis_rank_changed  = 0;  ///< elections whose elevation rank picked a different station than the coverage order would have, plus incumbents deposed by a flag.
    long long n_pb_basis_declared      = 0;  ///< handovers declared to the consumer (markSatDiscontinuity through the retro-bump queue), continuous and cold alike (RF-2 item 2).
    long long n_pb_basis_declared_cold = 0;  ///< …of which cold, i.e. the wire's phase bias stepped.
    // Why the datum moved: one counter per deposition cause (RF-2b).
    long long n_pb_basis_refuse_unusable = 0; ///< candidate evaluations refused because their carried level is not usable as a datum (the continuous branch's ledgerVerdict), the test that RF-2's rank lacked.
    long long n_pb_basis_elev_fallback   = 0; ///< elections where no candidate was ledger-usable and the rank fell back to elevation only (a cold pin).
    long long n_pb_basis_depose_flag     = 0; ///< incumbents deposed by a screen flag on their own basis column.
    long long n_pb_basis_depose_death    = 0; ///< …deposed because the basis arc has no carry left at all.
    long long n_pb_basis_depose_unusable = 0; ///< …deposed because their own carried level stopped passing the ledger verdict.
    long long n_pb_basis_value_drift   = 0;  ///< mid-tenure basis-value changes refused: the alarm of the held property itself.  Must read 0.
    long long n_cr_phase_datum_reref  = 0;  ///< phase-datum family re-reference
    /// Anchor-slip handover (network.cycle_slip.anchor_slip_handover;
    /// docs/SPEC_event_harness.md §12.7).  staged: directives the seam put up (families
    /// whose anchor arc a slip response freed).  handed: of those, the ones the election
    /// re-hosted.  continuous / cold: the successor pinned at its own carried value_abs,
    /// or pinned at 0 because no usable ledger entry survived the variance gates; a cold
    /// outcome is a gauge re-realization and is never folded into the continuous total.
    /// no_successor: the ban left no candidate, so the family went unpinned for the
    /// epoch.  inert: the directive matched no family in this epoch's union (gated,
    /// bridged or pruned column).  Armed with staged = 0 on a run whose detectors fired
    /// does not prove a quiet network; it is the first thing to check (invariant #4).
    /// staged > 0 with handed = 0 is dead wiring.
    long long n_anc_slip_staged = 0, n_anc_slip_handed = 0,
              n_anc_slip_continuous = 0, n_anc_slip_cold = 0,
              n_anc_slip_no_successor = 0, n_anc_slip_inert = 0,
              n_anc_slip_epochs = 0;
    /// Split-referenced satellite census (§14.5), always on and independent of the
    /// knob: per processed epoch, how many satellites have >= 2 anchored phase-bias
    /// families whose anchors sit at different stations.  Those satellites'
    /// per-frequency biases are realized against arc levels that walk independently,
    /// and this is the instrument that shows a split-reference regression.  It must read
    /// the same with the scope off, which is why it is not knob-gated.  Sum and epochs
    /// give the mean; the max is the worst epoch; the mean of multi_sum is the
    /// population that can split.
    long long n_cb_split_sats_sum = 0, n_cb_split_sats_max = 0,
              n_cb_split_multi_sum = 0, n_cb_split_epochs = 0;
    // The coverage gate's drop edge (network.phase_datum_gate_drop_erase;
    // the rule is network/solve/PhaseGateDrop.hpp).  Counted in both knob states, so an
    // off run reports the size of the class and an armed run reports how much of it the
    // response reached (standing invariant #4: a counter that exists only on the armed
    // path cannot say whether arming would matter).
    /// (sat,signal) families the gate excluded while they still held an anchor
    /// incumbent from the previous epoch.
    size_t n_pd_gate_drop_edges = 0;
    /// …of which the knob added to the re-reference set (= edges when armed with
    /// phase_anchor_carry_reref, 0 when off or inert).
    size_t n_pd_gate_drop_handled = 0;
    /// Per-station carried ambiguities erased because of a drop edge: the subset of the
    /// re-reference erase that the drop-edge response owns, separated from the
    /// handover-driven `n_cr_phase_datum_reref` population it shares the path with (and
    /// tagged `erase:gate_drop` in network_amb_state.csv).
    size_t n_pd_gate_drop_arcs = 0;
    // Run-scope observability for 1(d), as for eq34_reref_total: families marginalized
    // and phase-leveling local carries erased over the run.
    long long pa_reref_total = 0, pa_reref_erase_total = 0;
};

} // namespace GnssProcessor
