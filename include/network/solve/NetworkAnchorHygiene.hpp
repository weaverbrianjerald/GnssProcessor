/**
 * @file NetworkAnchorHygiene.hpp
 * @brief eq34 reattach hygiene: anchor-host eligibility, the time-linked phase
 *        datum ("pin bridge") and the fresh-pin deweight ramp.
 *        Card: docs/SPEC_eq34_reattach_hygiene.md.
 *
 * ## Problem
 *
 * The phase datum's per-(sat,signal) anchor is re-elected every epoch.  When the
 * incumbent host's arc ends, the key is re-hosted: under the Eq-34 chain datum by a
 * tree-edge reattach, under the prioritized election by an anchor handover.  At the
 * observability site (NetworkSolPrep.cpp) a reattach is taken to step the emitted δ̃_s
 * by an integer number of cycles (usually 0 under pin-to-round).  That holds only when
 * the new host's ambiguity has converged.  The float of a one-epoch-old arc is
 * code-seeded (± metres), so realizing the gauge on it re-levels the component by an
 * arbitrary amount.
 *
 * The failure is not an observability collapse: in the recorded field case the
 * smallest singular value of the normals was unchanged.  The re-levelling enters
 * through the right-hand side along the weak phase-bias subspace; the datum row holds
 * the pinned satellite's bias at the cold value, the other satellites' biases
 * re-express themselves around it, and the resulting between-satellite total_IF shift
 * persists.  The field case and its NEQ-SVD attribution are in the card.
 *
 * The existing QC does not see this event: RAIM T2 is carry_warm-gated (no opportunity
 * with leveling off), T3/T4 have no opportunity under carry_invariant, the clock-runaway
 * check is magnitude-degenerate on a single system, and an innovation gate never sees
 * a datum row.  The guard therefore lives in the election.
 *
 * ## What this seam does (three independently configured increments)
 *
 *  - **INC-1 `network.eq34_anchor_min_arc_ep`** (int, 0 = off): a (station, ambiguity)
 *    arc is an eligible anchor host only after it has been continuously present for
 *    that many epochs.  Two exemptions restrict the gate to re-hosting:
 *      * the incumbent host is always eligible (keeping it is not a reattach, and
 *        blocking it would cause the handover the gate exists to prevent), and
 *      * a first-sighting key (no incumbent, no bridge or hold, and no emitted
 *        realization within `network.eq34_prior_realization_max_gap_s`) is never
 *        gated, so cold start and new satellites are unaffected.
 *    With no eligible host the key is held out of the anchored set for the epoch.
 *    This is the disposition the coverage gate (`network.phase_datum_min_stations`)
 *    gives a thin key: it leaves `union_keys`, its per-station phase-bias states are
 *    pruned, and its bias folds into each station's float ambiguity, as for an
 *    unshared signal.
 *  - **INC-2 `network.phase_datum_time_linked`** (bool): instead of dropping, hold the
 *    key's shared phase bias at its last solved level with a soft pseudo-obs row
 *    (`phase_datum_bridge_sigma_m`) folded before the rank gate, the analogue of
 *    `code_datum_time_linked`.  The gauge for that satellite is then realized on the
 *    shared side rather than on a cold local ambiguity.  Standing invariant #1 (a stiff
 *    time-linked quantity needs a wired event response) is met: the bridge drops on a
 *    slip-funnel hit of its own key, on the epoch after a clock-datum change, on
 *    staleness (`phase_datum_bridge_max_ep`), and when the key leaves the shared set,
 *    each reason counted separately.  It also closes when an eligible host appears.
 *  - **INC-3 `network.eq34_fresh_pin_sigma_scale`** (double, 1.0 = off): when a pin on
 *    a fresh host is unavoidable, fold it with an inflated σ that decays linearly over
 *    `eq34_fresh_pin_ramp_ep`.
 *
 * ## Ordering contract
 *
 * `networkAnchorHygiene` runs
 *   * after `networkBuildPhaseDatumCandidates`: it consumes and modifies that census
 *     (`cnd.union_keys`) and relies on the coverage gate's phase-bias prune having
 *     already run,
 *   * after the PASS-A slip scans: the funnel (`carry_slipped_ambs`) must be complete
 *     for the epoch, or a just-slipped arc would present a mature age (invariant #1),
 *   * before the election (`networkElectChainDatum` / `networkElectPhaseAnchors`), whose
 *     host choice and pin set it constrains, and hence before the eq34 RPB allocation
 *     and `est.beginEpoch()` / PASS B.
 *
 * It owns no state: the arc-age map, the open bridges and the harvested levels live
 * in @ref NetworkCarryState, and every counter in @ref NetworkServiceCounters.
 * With `ctx.knobs.anchor_hygiene_on` false it resets the output verdicts to their
 * default and returns; the off path allocates no map and folds no row.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "network/state/NetworkEntities.hpp"   // net::ArcTable (card NOM step N4f1)
#include "network/driver/NetworkRunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct NetworkRunContext;   // app/NetworkRunContext.hpp defines it as a struct; the tag
                            // must match, or MSVC mangles the same free function two ways
                            // depending on which header a translation unit saw first
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief Run tallies for the card (standing invariant #4: run-end counters are the
 *        on-path telemetry).  Embedded in @c NetworkServiceCounters as `hyg`, so the
 *        eq34 election (which has the context) increments it directly and the anchor
 *        election (a pure function) takes it as an out-parameter.
 *
 * @c epochs is the denominator: an armed run reporting 0 passes never ran the gate,
 * which differs from a gate that ran and found nothing to block, and the log output
 * alone does not distinguish the two.  @c arcs_tracked is the same test one level
 * down (a live gate that never saw an arc).
 */
struct NetworkAnchorTallies {
    long long epochs = 0;                ///< hygiene passes run (denominator).
    long long arcs_tracked = 0;          ///< (station, ambiguity) arc-epochs aged.
    long long reattaches = 0;            ///< host actually moved (the guarded event).
    long long reattach_blocked_cold = 0; ///< refused: every candidate host too fresh (distinct holds entered).
    long long reattach_cold_allowed_no_prior = 0; ///< cold host allowed because no fresh emitted realization contradicted it (the restart / long-outage path); counted so it is visible.
    long long blocked_key_epochs = 0;    ///< key-epochs spent held out: the hold's duration, which the count of holds does not express.
    long long blocks_released = 0;       ///< holds released because a host converged.
    long long cold_fallback_pins = 0;    ///< fresh hosts used anyway (connectivity had no alternative).
    long long bridges_opened = 0;        ///< bridges opened (INC-2).
    long long bridge_folds = 0;          ///< bridge rows folded, in key-epochs (INC-2).
    long long bridge_drop_slip = 0;      ///< dropped: slip-funnel hit on the bridged key (invariant #1).
    long long bridge_drop_datum = 0;     ///< dropped: a clock-datum change retired the held value's frame.
    long long bridge_drop_stale = 0;     ///< dropped: folded for phase_datum_bridge_max_ep epochs.
    long long bridge_drop_leftset = 0;   ///< dropped: the key left this epoch's shared set.
    long long bridge_drop_healed = 0;    ///< closed: an eligible host appeared, so a normal pin resumes.
    long long fresh_pin_ramps = 0;       ///< datum rows folded with a ramped σ (INC-3).
    // --- Two-edge continuous handover (network.phase_datum_handover_continuous) ---
    // The two edges are counted separately: a handover that could not be made
    // continuous is a real re-realization that must be declared to the consumer, and
    // merging it into the count of continuous handovers would hide the gauge change
    // this mechanism exists to remove (invariant #7).
    long long handover_continuous = 0;   ///< Pin handed over at the successor's own outgoing-frame value; no frame step.
    long long handover_cold = 0;         ///< No ledger entry for the successor (fresh arc / first epoch): pinned at 0, and the frame steps.
    double    handover_max_abs_m = 0.0;  ///< Largest |target| applied: the size of the step that was avoided.
    long long handover_refused_var = 0;  ///< Ledger value refused as too ill-determined to serve as a datum (subset of handover_cold); invariant #7's re-validation on the re-coupling edge.
    long long handover_refused_ratio = 0; ///< Ledger value refused by the variance-ratio test var <= ratio × var_first (network.phase_datum_handover_max_var_ratio; subset of handover_cold, disjoint from handover_refused_var; an unknown var_first counts here).
    // --- Cold anchor fallback (network.phase_datum_candidacy_usable) ------------
    // docs/SPEC_event_harness.md §15.  Invariant #4: the pass is a candidacy screen,
    // so telling "it ran and changed the election" from "it ran and agreed with the
    // default sweep" and from "it never ran" needs all three counts.
    // `candidacy_rank_changed` is the main one: the number of elections where the
    // usable candidate is not the station the default sweep would have taken, which
    // is the population the card targets.
    long long candidacy_usable_elected = 0;      ///< Handovers whose successor came from the usable-first pass.
    long long candidacy_rank_changed = 0;        ///< ...and the elected station differs from the default sweep's pick (the pass changed the outcome).
    long long candidacy_no_usable_fallback = 0;  ///< Handovers where no candidate was usable, so the default sweeps and the cold path apply.
    long long candidacy_skipped_unusable = 0;    ///< (candidate, family) pairs the pass skipped: the sum of the four splits below.
    long long candidacy_skip_no_entry = 0;       ///< ...no ledger entry for the candidate at all (a never-carried arc).
    long long candidacy_skip_var_refused = 0;    ///< ...entry present but refused by the variance / ratio ceiling (or a forced-cold directive).
    long long candidacy_skip_freed = 0;          ///< ...the candidate's own arc was freed this epoch (leveling latch or DD slip funnel): its level is about to be re-drawn from code.
    long long candidacy_skip_young = 0;          ///< ...arc younger than phase_datum_candidacy_min_age_ep.
    // --- Iteration 2: the satellite clause (network.phase_datum_candidacy_satellite) --
    // docs/SPEC_event_harness.md §15.8.  The per-family screen of iteration 1 could elect
    // three hosts for a satellite's three bands; these count the joint per-satellite
    // resolution that replaces it.  `sat_clause_split_avoided` is the main one: the
    // number of families the family-only rule would have sent to a different station,
    // i.e. the split references this clause removed.
    long long sat_clause_satellites = 0;         ///< satellite-epochs resolved jointly (>= 1 family moving).
    long long sat_clause_families = 0;           ///< families whose successor came from the joint resolution.
    long long sat_clause_colocated = 0;          ///< ...where rule (b) decided it: the host already anchors the satellite's other families.
    long long sat_clause_split_avoided = 0;      ///< ...where the family-only rule would have picked a different station.
    long long sat_clause_cold_colocated = 0;     ///< families pinned at the joint host through the cold path (not usable there): a frame step, not a split.
    long long sat_clause_host_untracked = 0;     ///< families the joint host does not track, so they fall back to the per-family path (the only way this clause can still split).
    long long sat_clause_no_usable = 0;          ///< satellite-epochs where no candidate was usable for any moving family, so the default sweeps apply.
    double bridge_margin_decades = 0.0;  ///< log10 margin of the bridge row information over the rank-gate cutoff, sampled once (card KP, K2; minimum 2.0).
    bool   bridge_margin_logged = false; ///< one-shot latch for that check.
};

/**
 * @brief This epoch's anchor-host eligibility verdicts, handed to the election.
 *
 * A plain read-only view: @ref age aliases `the arc table's anchor_arc_age` (which the
 * seam rebuilt this epoch) and @ref bridged names the keys whose gauge is realized
 * by an INC-2 bridge row instead of a pin.  Default-constructed (`active == false`,
 * `min_arc_ep == 0`), every predicate returns the ungated answer, so an election
 * handed a default instance behaves as it does without the hygiene seam.
 */
struct AnchorHygiene {
    /// The seam ran this epoch (i.e. @c ctx.knobs.anchor_hygiene_on).
    bool active = false;
    /// Eligibility threshold in consecutive-present epochs; 0 disables the test (INC-1).
    int min_arc_ep = 0;
    /// Maximum σ multiplier on a fresh-host pin; 1.0 = off (INC-3).
    double fresh_pin_scale = 1.0;
    /// Ramp length in epochs; 0 = off (INC-3).
    int fresh_pin_ramp_ep = 0;
    /// The arc table, whose @c ArcState::anchor_arc_age the ages are read from (card NOM step
    /// N4f1); null when inactive.
    const net::ArcTable* age = nullptr;
    /// "sat_signal" keys realized by an open INC-2 bridge this epoch.  The election
    /// does not pin these (the bridge row is their datum) but keeps them in the
    /// shared set, and under eq34 treats them as spanning-forest roots.
    std::set<std::string> bridged;

    /// Consecutive-present epochs of arc (@p sta, @p amb); 0 when unknown/inactive.
    int arcAge(std::size_t sta, const std::string& amb) const {
        if (!age) return 0;
        const int* a = net::getArcAmb(*age, static_cast<int>(sta), amb, &net::ArcState::anchor_arc_age);
        return a == nullptr ? 0 : *a;
    }
    /// Whether arc (@p sta, @p amb) may host the datum for @p amb on a reattach.
    /// Always true when the gate is inactive or the threshold is 0.
    bool eligibleHost(std::size_t sta, const std::string& amb) const {
        return !active || min_arc_ep <= 0 || arcAge(sta, amb) >= min_arc_ep;
    }
    /// Is @p amb's gauge realized by an open bridge row this epoch?
    bool isBridged(const std::string& amb) const {
        return active && bridged.count(amb) != 0;
    }
    /**
     * @brief INC-3 σ multiplier for a datum pin on (@p sta, @p amb).
     *
     * 1.0 (no scaling, byte-identical fold) unless the ramp is armed and the host is
     * younger than `min_arc_ep + fresh_pin_ramp_ep`.  Inside the ramp the multiplier
     * decays linearly from @ref fresh_pin_scale at age ≤ min_arc_ep to 1 at the ramp
     * end, so a pin does not jump discontinuously from soft to hard.
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
 * @brief Age this epoch's anchorable arcs, decide the reattach verdicts, and run the
 *        INC-2 bridge lifecycle.  See the file banner for the ordering contract.
 *
 * @param ctx                 [in/out] arc-age map, open bridges, counters, knobs.
 * @param config_             read only for the arc-ledger switches
 *                            (networkLedgerScreens / networkLedgerAnchor, card SID);
 *                            the datum-mode predicates come from ctx.knobs.
 * @param log                 producer logger (info/debug).
 * @param traces              PHASE_DATUM `trace_events` rows.
 * @param t                   this epoch's time (event stamps).
 * @param stations            [in/out] non-const: a blocked key's phase-bias states are
 *                            pruned, as the coverage gate prunes those of a thin key.
 * @param present             this epoch's presence mask.
 * @param carry_slipped_ambs  the PASS-A slip funnel: (station index, ambiguity key)
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: phase-anchor hygiene: handover, cold fallback, PHASE_ANCHOR re-reference, eq34.
 *
 * The phase-anchor hygiene family: the two-edge continuous PB-gauge handover, the cold
 * anchor-fallback with its satellite clause, the PHASE_ANCHOR carry re-reference, and
 * the eq34 reattach hygiene with the INC-2 bridges.
 *
 * Gate: each section under its own condition, as in the teardown:
 * `ctx.knobs.pd_handover_continuous`; `ctx.knobs.pd_candidacy_usable`;
 * `ctx.knobs.pa_carry_reref`; `ctx.knobs.anchor_hygiene_on`.
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkAnchorHygieneRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

} // namespace GnssProcessor
