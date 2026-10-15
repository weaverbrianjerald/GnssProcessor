#pragma once
/**
 * @file NetworkSlipFunnel.hpp
 * @brief The slip funnel of one network epoch (card C3c): every (station roster index,
 *        ambiguity key) the epoch treats as slipped, its producers, its readers, and the
 *        standing invariants it serves (card NOM3 step EP2).
 *
 * The funnel is an epoch group (NetworkEpoch::funnel, app/NetworkEpoch.hpp): empty at the
 * epoch's construction, filled before the phase datum, read from the phase datum on, and gone
 * at the epoch's end.  A flag that must reach the next epoch is run state
 * (NetworkCarryState::carry_innov_pending, carry_innov_pending_flat), never a funnel entry.
 *
 * Producers, in spine order (each takes the funnel as an output parameter):
 *   1. networkSlipFunnelOpen: the previous epoch's innovation-gate flags (card 4 D5), consumed
 *      one-shot, and the retap-only drain beside them (carry_innov_flat_now).
 *   2. Application::networkPassASlipScan_ (ApplicationNetworkAssembly.cpp): the PASS-A slip
 *      detectors' drains (slip/NetworkSlipQc.cpp), into the raw subset only.
 *   3. networkSlipFunnelUnionRaw: the raw subset joins the funnel.
 *   4. networkSignalIntegrity (flicker regain refloats) and networkMeasurementAdmission (onset,
 *      heal and station-guard refloats): into both sets.
 * Readers: networkPhaseAnchorEpoch (the anchor hygiene's maturity, the chain datum, the
 * anchor-slip handover, the candidacy bar), networkGapResumeCensus (a marked arc is never
 * counted as bridged), Application::networkPassB_ (the T5 erase policy reads the raw subset),
 * networkBuildCarryDdPriors and the solve glue's replay rebuild (the DD carry's membership
 * exclusion: a funnel arc re-taps fresh), networkEmitStore, networkSignalRosterAudit.  The DD
 * build and the solve glue take the funnel as a writable parameter and only read it.
 *
 * The standing invariants it serves (CLAUDE.md, "Standing invariants"; card NOM section 3.5):
 *   - Invariant 1, every stiff carried quantity has a wired slip response: the funnel is the
 *     response's feed for the DD carry (a funnel arc leaves A_old and is re-born), the phase
 *     anchor (a just-freed arc is not mature, a freed anchor hands over) and the T5 deviation
 *     (a raw-confirmed slip erases).
 *   - Invariant 7, a change in which information reaches which states is an event at both
 *     edges, with counters and identities: each entry carries its identity (station, ambiguity
 *     key) and is the decoupling edge of that arc for this epoch.
 * Enforced here, by construction and without an output change: the deferred innovation flags
 * are read and cleared only by networkSlipFunnelOpen, once per processed epoch (no flag is
 * applied twice or lost to a skipped epoch, since a skipped epoch never opens a funnel), and
 * after networkSlipFunnelUnionRaw the raw subset is contained in the funnel (every later
 * producer inserts into both).  Named and NOT enforced, because enforcing either adds output
 * (a counter or a run-end line): (a) a per-entry check that every carried quantity keyed by a
 * funnel entry was dropped, refloated or re-seeded by the epoch's end (invariant 1); (b) per
 * identity counters of the funnel's entries and of the arcs' re-coupling (invariant 7; the
 * re-coupling edge is the gap-resume census and the arc ledger's, not the funnel's).
 */
#include <set>
#include <string>
#include <utility>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;

/// The slip funnel (card C3c).  See the file comment for its producers, readers and invariants.
struct NetworkSlipFunnel {
    std::set<std::pair<int, std::string>> carry_slipped_ambs; ///< The funnel: (station roster index, ambiguity key).
    std::set<std::pair<int, std::string>> carry_slipped_raw;  ///< Its raw-detector subset (Iono-2 v3k): the entries a raw observable saw step.
};

/**
 * @brief Opens the funnel: the previous epoch's deferred innovation-gate flags, one-shot.
 *
 * Under network.carry_invariant_innov_gate the flags the gate raised after the last solve
 * (ctx.carry.carry_innov_pending) enter the funnel and are cleared.  The retap-only drain
 * (card 4 D5 v4) moves the corroborated-flat subset into ctx.carry.carry_innov_flat_now, which
 * is cleared first on every epoch so a disarmed run cannot inherit a stale set.
 *
 * @param run    The run: reads ctx.knobs.carry_innov_gate; writes ctx.carry's pending and
 *               flat sets.
 * @param ep     The epoch (not read; the epoch form).
 * @param funnel The funnel (written: carry_slipped_ambs).
 */
void networkSlipFunnelOpen(NetworkRun& run, const NetworkEpoch& ep, NetworkSlipFunnel& funnel);

/**
 * @brief Folds the raw subset into the funnel, after PASS A's slip scans.
 *
 * The PASS-A drains insert only into carry_slipped_raw; the union preserves the funnel's
 * previous contents, so every reader sees the same set it saw when the drains wrote the funnel
 * directly.
 *
 * @param run    The run (not read; the epoch form).
 * @param ep     The epoch (not read; the epoch form).
 * @param funnel The funnel (written: carry_slipped_ambs; read: carry_slipped_raw).
 */
void networkSlipFunnelUnionRaw(NetworkRun& run, const NetworkEpoch& ep, NetworkSlipFunnel& funnel);

} // namespace GnssProcessor
