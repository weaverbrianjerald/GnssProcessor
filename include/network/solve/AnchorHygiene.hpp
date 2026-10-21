/**
 * @file AnchorHygiene.hpp
 * @brief Anchor-host hygiene for the network phase datum: which arcs may host a
 *        re-hosted anchor, the time-linked phase datum ("pin bridge") and the
 *        fresh-pin deweight ramp.
 *
 * ## Problem
 *
 * The phase datum anchors each (satellite, signal) phase bias (PB) on one station's
 * ambiguity, and the anchor is re-elected every epoch. When the incumbent host's arc
 * ends, the key is re-hosted: by a tree-edge reattach under the chain datum
 * (`network.phase_datum_eq34`, "eq34"), or by an anchor handover under the prioritized
 * election. At the observability site (SolPrep.cpp) a reattach is taken to step the
 * emitted satellite bias δ̃_s by an integer number of cycles (usually 0 under
 * pin-to-round). That holds only when the new host's ambiguity has converged. The float
 * ambiguity of a one-epoch-old arc is seeded from code (error of metres), so realizing
 * the gauge on it re-levels the whole component by an arbitrary amount.
 *
 * The failure is not a loss of observability: in the field case that exposed it, the
 * smallest singular value of the normal matrix was unchanged. The re-levelling enters
 * through the right-hand side along the weak phase-bias subspace. The datum row holds
 * the pinned satellite's bias at the cold value, the other satellites' biases
 * re-express themselves around it, and the resulting shift of the between-satellite
 * ionosphere-free (IF) total (`total_IF`) persists.
 *
 * Existing quality control does not see this event:
 *  - the within-epoch RAIM response (receiver autonomous integrity monitoring, tier T2)
 *    runs only with phase leveling warm (`carry_warm`), so it has no opportunity with
 *    leveling off;
 *  - the consensus and attribution tiers (T3, T4) have no opportunity under
 *    `carry_invariant`;
 *  - the clock-runaway check cannot resolve a magnitude on a single system;
 *  - an innovation gate never sees a datum row.
 *
 * The guard therefore lives in the election.
 *
 * ## What this seam does (three independently configured mechanisms)
 *
 *  - **Eligibility gate `network.eq34_anchor_min_arc_ep`** (int, 0 = off): a
 *    (station, ambiguity) arc is an eligible anchor host only after it has been
 *    continuously present for that many epochs. Two exemptions restrict the gate to
 *    re-hosting:
 *      * the incumbent host is always eligible (keeping it is not a reattach, and
 *        blocking it would cause the handover the gate exists to prevent), and
 *      * a first-sighting key (no incumbent, no bridge or hold, and no emitted
 *        realization within `network.eq34_prior_realization_max_gap_s`) is never
 *        gated, so cold start and new satellites are unaffected.
 *
 *    With no eligible host the key is held out of the anchored set for the epoch.
 *    This is the disposition the coverage gate (`network.phase_datum_min_stations`)
 *    gives a thin key: it leaves `union_keys`, its per-station phase-bias states are
 *    pruned, and its bias folds into each station's float ambiguity, as for an
 *    unshared signal.
 *  - **Pin bridge `network.phase_datum_time_linked`** (bool): instead of dropping the
 *    key, hold its shared phase bias at its last solved level with a soft
 *    pseudo-observation row (`network.phase_datum_bridge_sigma_m`) folded before the
 *    rank gate. It is the analogue of `network.code_datum_time_linked`. The gauge for
 *    that satellite is then realized on the shared side rather than on a cold local
 *    ambiguity. A stiff time-linked quantity needs a response to every jump, so the
 *    bridge is dropped on a slip-funnel hit of its own key, on the epoch after a
 *    clock-datum change, on staleness (`network.phase_datum_bridge_max_ep`) and when
 *    the key leaves the shared set. Each reason is counted separately. The bridge also
 *    closes when an eligible host appears.
 *  - **Fresh-pin ramp `network.eq34_fresh_pin_sigma_scale`** (double, 1.0 = off): when
 *    a pin on a fresh host is unavoidable, fold it with an inflated σ that decays
 *    linearly to the normal value over `network.eq34_fresh_pin_ramp_ep` epochs.
 *
 * ## Ordering contract
 *
 * `networkAnchorHygiene` runs
 *   * after `networkBuildPhaseDatumCandidates`: it consumes and modifies that census
 *     (`cnd.union_keys`) and relies on the coverage gate's phase-bias prune having
 *     already run,
 *   * after the PASS A slip scans: the slip funnel (`carry_slipped_ambs`) must be
 *     complete for the epoch, or a just-slipped arc would present a mature age,
 *   * before the election (`networkElectChainDatum` / `networkElectPhaseAnchors`),
 *     whose host choice and pin set it constrains, and hence before the eq34
 *     receiver phase-bias allocation and `est.beginEpoch()` / PASS B.
 *
 * It owns no state. Arc ages live in @c net::ArcState::anchor_arc_age, open bridges and
 * harvested levels in @c net::SatSignalState::pin_bridge and @c pb_level_prev, the
 * hold-out set in @c NetworkCarryState::anchor_blocked, and every counter in
 * @ref NetworkServiceCounters. With `ctx.knobs.anchor_hygiene_on` false the seam resets
 * the output verdicts to their default and returns, allocating no map and folding no
 * row.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "network/state/Entities.hpp"   // net::ArcTable
#include "network/driver/RunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
// Defined as a struct in network/state/RunContext.hpp. The tag must match, or MSVC
// mangles the same free function two ways depending on which header a translation
// unit saw first.
struct NetworkRunContext;
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief Run tallies for the hygiene seam.
 *
 * Embedded in @c NetworkServiceCounters as `hyg`, so the eq34 election (which has the
 * context) increments it directly and the anchor election (a pure function) takes it as
 * an out-parameter. The tallies are the evidence that an armed mechanism actually ran,
 * so the run-end report reads them rather than the log.
 *
 * @c epochs is the denominator: an armed run reporting 0 passes never ran the gate,
 * which differs from a gate that ran and found nothing to block, and the log output
 * alone does not distinguish the two. @c arcs_tracked is the same test one level
 * down (a live gate that never saw an arc).
 */
struct NetworkAnchorTallies {
    long long epochs = 0;                ///< hygiene passes run (denominator).
    long long arcs_tracked = 0;          ///< (station, ambiguity) arc-epochs aged.
    long long reattaches = 0;            ///< host actually moved (the guarded event); counted on every handover of a prioritized election, but under the eq34 chain datum only while anchor_hygiene_on.
    long long reattach_blocked_cold = 0; ///< refused: every candidate host too fresh (distinct holds entered).
    long long reattach_cold_allowed_no_prior = 0; ///< cold host allowed because no fresh emitted realization contradicted it (the restart / long-outage path); counted so it stays visible.
    long long blocked_key_epochs = 0;    ///< key-epochs spent held out: the hold's duration, which the count of holds does not express.
    long long blocks_released = 0;       ///< holds released because a host converged.
    long long cold_fallback_pins = 0;    ///< fresh hosts used anyway (connectivity had no alternative).
    long long bridges_opened = 0;        ///< pin bridges opened.
    long long bridge_folds = 0;          ///< bridge rows folded, in key-epochs.
    long long bridge_drop_slip = 0;      ///< dropped: slip-funnel hit on the bridged key.
    long long bridge_drop_datum = 0;     ///< dropped: a clock-datum change retired the held value's frame.
    long long bridge_drop_stale = 0;     ///< dropped: folded for phase_datum_bridge_max_ep epochs.
    long long bridge_drop_leftset = 0;   ///< dropped: the key left this epoch's shared set.
    long long bridge_drop_healed = 0;    ///< closed: an eligible host appeared, so a normal pin resumes.
    long long fresh_pin_ramps = 0;       ///< datum rows folded with a ramped σ.
    // --- Continuous handover (network.phase_datum_handover_continuous) ---
    // A handover pins the successor host at its own level from the handover ledger (the
    // record of each arc's last solved level and variance). Handovers that stay
    // continuous and those that cannot are counted separately: a non-continuous handover
    // is a real re-realization of the datum that must be declared to the consumer, and
    // merging it into the continuous count would hide the gauge change this mechanism
    // exists to remove.
    long long handover_continuous = 0;   ///< Pin handed over at the successor's own value in the outgoing frame; no frame step.
    long long handover_cold = 0;         ///< No ledger entry for the successor (fresh arc or first epoch): pinned at 0, and the frame steps.
    double    handover_max_abs_m = 0.0;  ///< Largest |target| applied: the size of the step that was avoided.
    long long handover_refused_var = 0;  ///< Ledger value refused as too poorly determined to serve as a datum (subset of handover_cold). The level is re-validated before it reconnects the successor.
    long long handover_refused_ratio = 0; ///< Ledger value refused by the variance-ratio test var <= ratio × var_first, where var_first is the arc's first recorded variance (network.phase_datum_handover_max_var_ratio; subset of handover_cold, disjoint from handover_refused_var; an unknown var_first counts here).
    // --- Cold anchor fallback (network.phase_datum_candidacy_usable) ------------
    // A usable-first candidacy pass elects a successor whose ledger level is usable,
    // instead of the first candidate of the default coverage sweep. It is a screen, so
    // telling "ran and changed the election" from "ran and agreed with the default
    // sweep" and from "never ran" needs all three counts. `candidacy_rank_changed` is
    // the main one: the number of elections where the usable candidate is not the
    // station the default sweep would have taken, which is the population this
    // fallback targets.
    long long candidacy_usable_elected = 0;      ///< Handovers whose successor came from the usable-first pass.
    long long candidacy_rank_changed = 0;        ///< ...and the elected station differs from the default sweep's pick (the pass changed the outcome).
    long long candidacy_no_usable_fallback = 0;  ///< Handovers where no candidate was usable, so the default sweeps and the cold path apply.
    long long candidacy_skipped_unusable = 0;    ///< (candidate, family) pairs the pass skipped: the sum of the four splits below.
    long long candidacy_skip_no_entry = 0;       ///< ...no ledger entry for the candidate at all (a never-carried arc).
    long long candidacy_skip_var_refused = 0;    ///< ...entry present but refused by the variance / ratio ceiling (or a forced-cold directive).
    long long candidacy_skip_freed = 0;          ///< ...the candidate's own arc was freed this epoch (leveling latch or DD slip funnel): its level is about to be re-drawn from code.
    long long candidacy_skip_young = 0;          ///< ...arc younger than phase_datum_candidacy_min_age_ep.
    // --- Satellite clause (network.phase_datum_candidacy_satellite) --------------
    // The per-family screen above could elect three hosts for a satellite's three bands.
    // These tallies count the joint per-satellite resolution that replaces it.
    // `sat_clause_split_avoided` is the main one: the number of families the family-only
    // rule would have sent to a different station, i.e. the split references this
    // clause removed.
    long long sat_clause_satellites = 0;         ///< satellite-epochs resolved jointly (>= 1 family moving).
    long long sat_clause_families = 0;           ///< families whose successor came from the joint resolution.
    long long sat_clause_colocated = 0;          ///< ...where the held-host preference decided it: the host already anchors the satellite's other families.
    long long sat_clause_split_avoided = 0;      ///< ...where the family-only rule would have picked a different station.
    long long sat_clause_cold_colocated = 0;     ///< families pinned at the joint host through the cold path (not usable there): a frame step, not a split.
    long long sat_clause_host_untracked = 0;     ///< families the joint host does not track, so they fall back to the per-family path (the only way this clause can still split).
    long long sat_clause_no_usable = 0;          ///< satellite-epochs where no candidate was usable for any moving family, so the default sweeps apply.
    double bridge_margin_decades = 0.0;  ///< log10 margin of the bridge row information over the rank-gate cutoff, sampled once (minimum 2.0).
    bool   bridge_margin_logged = false; ///< one-shot latch for that check.
};

/**
 * @brief This epoch's anchor-host eligibility verdicts, handed to the election.
 *
 * A plain read-only view. @ref age points at the arc table whose
 * @c ArcState::anchor_arc_age the seam rebuilt this epoch, and @ref bridged names the
 * keys whose gauge is realized by a pin-bridge row instead of a pin. Default-constructed
 * (`active == false`, `min_arc_ep == 0`), every predicate returns the ungated answer, so
 * an election handed a default instance behaves as it does without the hygiene seam.
 */
struct AnchorHygiene {
    /// The seam ran this epoch (i.e. @c ctx.knobs.anchor_hygiene_on).
    bool active = false;
    /// Eligibility threshold in consecutive-present epochs; 0 disables the test.
    int min_arc_ep = 0;
    /// Maximum σ multiplier on a fresh-host pin; 1.0 = off.
    double fresh_pin_scale = 1.0;
    /// Fresh-pin ramp length in epochs; 0 = off.
    int fresh_pin_ramp_ep = 0;
    /// The arc table, whose @c ArcState::anchor_arc_age the ages are read from;
    /// null when inactive.
    const net::ArcTable* age = nullptr;
    /// "sat_signal" keys realized by an open pin bridge this epoch. The election does
    /// not pin these (the bridge row is their datum) but keeps them in the shared set,
    /// and under eq34 treats them as spanning-forest roots.
    std::set<std::string> bridged;

    /**
     * @brief Consecutive-present epochs of an arc.
     * @param sta Station index.
     * @param amb "sat_signal" ambiguity key.
     * @return The arc's age in epochs; 0 when unknown or when the seam is inactive.
     */
    int arcAge(std::size_t sta, const std::string& amb) const {
        if (!age) return 0;
        const int* a = net::getArcAmb(*age, static_cast<int>(sta), amb, &net::ArcState::anchor_arc_age);
        return a == nullptr ? 0 : *a;
    }
    /**
     * @brief Whether an arc may host the datum for its ambiguity on a reattach.
     * @param sta Station index.
     * @param amb "sat_signal" ambiguity key.
     * @return true when the arc has reached `min_arc_ep`; always true when the seam is
     *         inactive or the threshold is 0.
     */
    bool eligibleHost(std::size_t sta, const std::string& amb) const {
        return !active || min_arc_ep <= 0 || arcAge(sta, amb) >= min_arc_ep;
    }
    /**
     * @brief Whether a key's gauge is realized by an open bridge row this epoch.
     * @param amb "sat_signal" ambiguity key.
     * @return true when the key is bridged.
     */
    bool isBridged(const std::string& amb) const {
        return active && bridged.count(amb) != 0;
    }
    /**
     * @brief σ multiplier for a datum pin on an arc (the fresh-pin ramp).
     *
     * 1.0 (the fold is unchanged) unless the ramp is armed and the host is younger than
     * `min_arc_ep + fresh_pin_ramp_ep`. Inside the ramp the multiplier decays linearly
     * from @ref fresh_pin_scale at age <= `min_arc_ep` to 1 at the end of the ramp, so a
     * pin does not jump discontinuously from soft to hard.
     *
     * @param sta Station index of the pin's host.
     * @param amb "sat_signal" ambiguity key.
     * @return The multiplier, in [1, fresh_pin_scale].
     */
    double pinSigmaScale(std::size_t sta, const std::string& amb) const {
        if (!active || fresh_pin_ramp_ep <= 0 || !(fresh_pin_scale > 1.0)) return 1.0;
        const int age_ep = arcAge(sta, amb);
        const int lo = min_arc_ep;                     // fully fresh at/below this
        const int hi = min_arc_ep + fresh_pin_ramp_ep; // fully mature at/above this
        if (age_ep >= hi) return 1.0;
        if (age_ep <= lo) return fresh_pin_scale;
        const double f = static_cast<double>(hi - age_ep)
                       / static_cast<double>(fresh_pin_ramp_ep);
        return 1.0 + (fresh_pin_scale - 1.0) * f;
    }
};

/**
 * @brief Age this epoch's anchorable arcs, decide the reattach verdicts and run the
 *        pin-bridge lifecycle.  See the file banner for the ordering contract.
 *
 * @param ctx                 [in/out] arc-age map, open bridges, counters, knobs.
 * @param config_             read only for the arc-ledger switches
 *                            (networkLedgerScreens / networkLedgerAnchor);
 *                            the datum-mode predicates come from ctx.knobs.
 * @param log                 producer logger (info/debug).
 * @param traces              PHASE_DATUM `trace_events` rows.
 * @param t                   this epoch's time (event stamps).
 * @param stations            [in/out] non-const: a blocked key's phase-bias states are
 *                            pruned, as the coverage gate prunes those of a thin key.
 * @param present             this epoch's presence mask.
 * @param carry_slipped_ambs  the PASS A slip funnel: (station index, ambiguity key)
 *                            pairs; unioned with each station's pending-reseed flags
 *                            so the age reset covers both slip feeds.
 * @param cnd                 [in/out] the candidate census; blocked keys are erased
 *                            from @c cnd.union_keys before the election reads it.
 * @param hyg                 [out] this epoch's verdicts (fully overwritten).
 */
void networkAnchorHygiene(NetworkRunContext& ctx,
                          const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces,
                          const GnssTime& t,
                          std::vector<NetStation>& stations,
                          const std::vector<bool>& present,
                          const std::set<std::pair<int, std::string>>& carry_slipped_ambs,
                          PhaseDatumCandidates& cnd,
                          AnchorHygiene& hyg);

// ---- Run-end reports ----

struct NetworkRunEndView;

/**
 * @brief Run-end report for phase-anchor hygiene.
 *
 * Four sections, each printed under its own knob:
 *  - the continuous PB-gauge handover (`ctx.knobs.pd_handover_continuous`),
 *  - the cold anchor fallback with its satellite clause (`ctx.knobs.pd_candidacy_usable`),
 *  - the PHASE_ANCHOR carry re-reference (`ctx.knobs.pa_carry_reref`),
 *  - the eq34 reattach hygiene with its pin bridges (`ctx.knobs.anchor_hygiene_on`).
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkAnchorHygieneRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

/// @brief Run-scope counters of AnchorHygiene, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct AnchorHygieneCounters {
    // Run tallies of the anchor hygiene family; every field is documented on the
    // struct. The eligibility, bridge and ramp tallies move only while
    // ctx.knobs.anchor_hygiene_on; the handover, candidacy and satellite-clause tallies
    // move under their own knobs. `reattaches` is the exception: the prioritized
    // election counts it on every anchor handover whether or not anchor_hygiene_on is
    // set (the eq34 chain datum counts it only while that knob is set), because the
    // handover and re-reference run-end sections use it as their denominator. All are
    // reported by networkAnchorHygieneRunEnd (network/solve/AnchorHygiene.cpp).
    NetworkAnchorTallies hyg;
};

} // namespace GnssProcessor
