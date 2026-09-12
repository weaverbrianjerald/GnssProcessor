/**
 * @file NetworkAnchorHygiene.hpp
 * @brief eq34 REATTACH HYGIENE — anchor-host eligibility, the time-linked phase
 *        datum ("pin bridge") and the fresh-pin deweight ramp.
 *        Card: docs/SPEC_eq34_reattach_hygiene.md.
 *
 * ## The defect
 *
 * The phase datum's per-(sat,signal) anchor is re-elected every epoch.  When the
 * incumbent host's arc dies, the key is RE-HOSTED — under the Eq-34 chain datum a
 * tree-edge reattach, under the prioritized election an anchor handover.  The
 * in-code assumption at the observability site (NetworkSolPrep.cpp) was that a
 * reattach steps the emitted δ̃_s "by an EXACT integer number of cycles … usually 0
 * under pin-to-round".  That is true only when the NEW host's ambiguity has
 * CONVERGED.  A one-epoch-old arc's float is code-seeded (± metres), so realizing
 * the gauge on it re-levels the component by an arbitrary amount.
 *
 * Measured (DOY-129 2024-05-08, FT63 24 h all-on): at 19:25:30 the event
 * `PHASE_DATUM/eq34_reattach G29 GAMB00PYF->INEG00MEX` re-hosted G29's anchor onto
 * INEG's ONE-EPOCH-OLD arc (G29 rising after a 2138-epoch gap).  The epoch itself
 * solved cleanly (post-fit phase σ̂0 2.95).  One epoch later the whole field broke:
 * a00 SUM 38.7 → 5739 TECU, σ̂0 → 56.7, every satellite's code residuals blown, the
 * SH health gate engaged for 76 min, and an across-gap between-satellite total_IF
 * re-shuffle of 181 m that NEVER healed.  The NEQ-SVD attribution at the break pair
 * shows σ_min UNCHANGED (2.97e-2 → 2.95e-2) — this is NOT an observability collapse:
 * the RHS loading along the weak phase-bias subspace swings +33 → −2733 and Δx is
 * phase-bias-led, with G29's own bias QUIET.  G29 is the PIN: the datum row fixed it
 * at the cold value and the bias constellation re-expressed itself around it.
 *
 * Nothing caught it by construction: RAIM T2 is carry_warm-gated (zero-opportunity
 * with leveling off), T3/T4 zero-opportunity under carry_invariant, the clock-runaway
 * check is magnitude-degenerate single-system, and an innovation gate never sees a
 * DATUM row.  The guard therefore has to live in the ELECTION.
 *
 * ## What this seam does (three independently-knobbed increments)
 *
 *  - **INC-1 `network.eq34_anchor_min_arc_ep`** (int, 0 = off): a (station, ambiguity)
 *    arc is an ELIGIBLE anchor host only once it has been continuously present for
 *    that many epochs.  Two exemptions keep the gate aimed at re-hosting only:
 *      * the INCUMBENT host is always eligible (keeping it is not a reattach — and
 *        blocking it would CAUSE the handover the card exists to prevent), and
 *      * a FIRST-SIGHTING key (no incumbent at all) is never gated, so cold start
 *        and every genuinely new satellite are untouched.
 *    With no eligible host the key is held OUT of the anchored set for the epoch —
 *    the same disposition the coverage gate (`network.phase_datum_min_stations`)
 *    already gives a thin key: it leaves `union_keys`, its per-station phase-bias
 *    states are pruned, and its bias folds into each station's float ambiguity
 *    exactly as an unshared signal's always has.
 *  - **INC-2 `network.phase_datum_time_linked`** (bool): instead of dropping, hold the
 *    key's shared phase bias at its last solved level with a SOFT pseudo-obs row
 *    (`phase_datum_bridge_sigma_m`) folded before the rank gate — the
 *    `code_datum_time_linked` analogue.  The gauge is then realized on the SHARED
 *    side for that one satellite rather than on a cold local ambiguity.  Standing
 *    invariant #1 (a stiff time-linked quantity MUST have a wired event response) is
 *    honoured: the bridge drops on a slip-funnel hit of its own key, on the epoch
 *    after a clock-datum change, on staleness (`phase_datum_bridge_max_ep`), and when
 *    the key leaves the shared set — each reason counted separately.
 *  - **INC-3 `network.eq34_fresh_pin_sigma_scale`** (double, 1.0 = off): when a pin on
 *    a fresh host is unavoidable, fold it with an inflated σ that decays linearly over
 *    `eq34_fresh_pin_ramp_ep`.
 *
 * ## Ordering contract
 *
 * `networkAnchorHygiene` MUST run
 *   * AFTER `networkBuildPhaseDatumCandidates` — it consumes and MUTATES that census
 *     (`cnd.union_keys`) and relies on the coverage gate's phase-bias prune having
 *     already happened, and
 *   * AFTER the PASS-A slip scans — the funnel (`carry_slipped_ambs`) must be complete
 *     for the epoch or a just-slipped arc would present a mature age (invariant #1), and
 *   * BEFORE the election (`networkElectChainDatum` / `networkElectPhaseAnchors`), whose
 *     host choice and pin set it constrains, and hence before the eq34 RPB allocation
 *     and `est.beginEpoch()` / PASS B.
 *
 * It owns NO state: the arc-age map, the open bridges and the harvested levels all
 * live in @ref NetworkCarryState, and every counter in @ref NetworkServiceCounters.
 * With `ctx.knobs.anchor_hygiene_on` false it returns immediately, having touched
 * nothing — the OFF path allocates no map and folds no row.
 */
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "filter/NetworkRunTypes.hpp"

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
class NetworkRunContext;
class NetworkRunTraces;
struct GnssTime;

/**
 * @brief Run tallies for the card (standing invariant #4 — counters or it didn't
 *        happen).  Embedded in @c NetworkServiceCounters as `hyg`, so the eq34
 *        election (which has the context) bumps it directly and the anchor election
 *        (a pure function) takes it as an out-parameter.
 *
 * @c epochs is the DENOMINATOR: an armed run reporting 0 election passes never ran
 * the gate at all — which is NOT the same as "the gate ran and found nothing to
 * block", and no amount of clean-looking log output would reveal it.  @c arcs_tracked
 * is the same test one level down (a live gate that never saw an arc).
 */
struct NetworkAnchorTallies {
    long long epochs = 0;                ///< hygiene passes run (denominator).
    long long arcs_tracked = 0;          ///< (station, ambiguity) arc-epochs aged.
    long long reattaches = 0;            ///< host actually moved (the guarded event).
    long long reattach_blocked_cold = 0; ///< refused: every candidate host too fresh (distinct HOLDS entered).
    long long reattach_cold_allowed_no_prior = 0; ///< cold host ALLOWED because no fresh emitted realization contradicted it — the restart / long-outage path, counted so it is never silent.
    long long blocked_key_epochs = 0;    ///< key-epochs spent held out — the hold's DURATION, which the count of holds cannot express.
    long long blocks_released = 0;       ///< holds released because a host finally converged.
    long long cold_fallback_pins = 0;    ///< fresh hosts used anyway (connectivity had no alternative).
    long long bridges_opened = 0;        ///< INC-2 bridges opened.
    long long bridge_folds = 0;          ///< INC-2 bridge rows folded (key-epochs).
    long long bridge_drop_slip = 0;      ///< dropped: slip-funnel hit on the bridged key (invariant #1).
    long long bridge_drop_datum = 0;     ///< dropped: a clock-datum change retired the held value's frame.
    long long bridge_drop_stale = 0;     ///< dropped: folded for phase_datum_bridge_max_ep epochs.
    long long bridge_drop_leftset = 0;   ///< dropped: the key left this epoch's shared set.
    long long bridge_drop_healed = 0;    ///< closed: an eligible host appeared, so a normal pin resumes.
    long long fresh_pin_ramps = 0;       ///< INC-3 datum rows folded with a ramped σ.
    // --- TWO-EDGE CONTINUOUS HANDOVER (network.phase_datum_handover_continuous) ---
    // Both edges are counted separately and deliberately: a handover that could
    // not be made continuous is a REAL re-realization the consumer must be told
    // about, and burying it in the same counter as a clean one would reproduce
    // the silent gauge change this mechanism exists to remove (invariant #7).
    long long handover_continuous = 0;   ///< Pin handed over at the successor's own outgoing-frame value — no frame step.
    long long handover_cold = 0;         ///< No ledger entry for the successor (fresh arc / first epoch): pinned at 0, frame DOES step.
    double    handover_max_abs_m = 0.0;  ///< Largest |target| applied — the size of the step that was AVOIDED.
    long long handover_refused_var = 0;  ///< Ledger value REFUSED as too ill-determined to serve as a datum (subset of handover_cold) — invariant #7's re-coupling re-validation.
    long long handover_refused_ratio = 0; ///< Ledger value REFUSED by the honest-σ-scale test var <= ratio × var_first (network.phase_datum_handover_max_var_ratio; subset of handover_cold, disjoint from handover_refused_var; an unknown var_first counts here).
    // --- COLD ANCHOR-FALLBACK (network.phase_datum_candidacy_usable) ------------
    // docs/SPEC_event_harness.md §15.  Invariant #4: the pass is a CANDIDACY screen,
    // so the only way to tell "it ran and changed the election" from "it ran and
    // agreed with today's sweep" from "it never ran" is to count all three.
    // `candidacy_rank_changed` is the load-bearing one — it is the number of
    // elections where the usable candidate is NOT the station today's sweep would
    // have taken, i.e. the population the card exists for.
    long long candidacy_usable_elected = 0;      ///< Handovers whose successor came from the usable-first pass.
    long long candidacy_rank_changed = 0;        ///< ...and the elected station DIFFERS from today's pick (the pass actually did something).
    long long candidacy_no_usable_fallback = 0;  ///< Handovers where NO candidate was usable → today's sweeps, honest cold path.
    long long candidacy_skipped_unusable = 0;    ///< (candidate, family) pairs the pass skipped — the sum of the four splits below.
    long long candidacy_skip_no_entry = 0;       ///< ...no ledger entry for the candidate at all (a never-carried arc).
    long long candidacy_skip_var_refused = 0;    ///< ...entry present but refused by the variance / ratio ceiling (or a forced-cold directive).
    long long candidacy_skip_freed = 0;          ///< ...the candidate's own arc was FREED this epoch (leveling latch or DD slip funnel): its level is about to be re-drawn from code.
    long long candidacy_skip_young = 0;          ///< ...arc younger than phase_datum_candidacy_min_age_ep.
    double bridge_margin_decades = 0.0;  ///< K2-style margin of the bridge information over the rank-gate cutoff.
    bool   bridge_margin_logged = false; ///< one-shot latch for that check.
};

/**
 * @brief This epoch's anchor-host eligibility verdicts, handed to the election.
 *
 * A plain read-only view: @ref age aliases `ctx.carry.anchor_arc_age` (which the
 * seam rebuilt this epoch) and @ref bridged names the keys whose gauge is realized
 * by an INC-2 bridge row instead of a pin.  Default-constructed (`active == false`,
 * `min_arc_ep == 0`) every predicate degrades to the legacy answer, so an election
 * handed a default instance behaves exactly as it did before this card.
 */
struct AnchorHygiene {
    /// The seam ran this epoch (i.e. @c ctx.knobs.anchor_hygiene_on).
    bool active = false;
    /// INC-1 threshold in consecutive-present epochs; 0 disables the eligibility test.
    int min_arc_ep = 0;
    /// INC-3 maximum σ multiplier on a fresh-host pin (1.0 = off).
    double fresh_pin_scale = 1.0;
    /// INC-3 ramp length in epochs (0 = off).
    int fresh_pin_ramp_ep = 0;
    /// Alias of `ctx.carry.anchor_arc_age`; null when inactive.
    const std::map<std::pair<int, std::string>, int>* age = nullptr;
    /// "sat_signal" keys realized by an open INC-2 bridge THIS epoch.  The election
    /// must NOT pin these (the bridge row is their datum) but must keep them in the
    /// shared set, and — under eq34 — must treat them as spanning-forest ROOTS.
    std::set<std::string> bridged;

    /// Consecutive-present epochs of arc (@p sta, @p amb); 0 when unknown/inactive.
    int arcAge(std::size_t sta, const std::string& amb) const {
        if (!age) return 0;
        const auto it = age->find({static_cast<int>(sta), amb});
        return it == age->end() ? 0 : it->second;
    }
    /// May arc (@p sta, @p amb) HOST the datum for @p amb on a reattach?  Always true
    /// when the gate is inactive or the threshold is 0 (legacy).
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
     * 1.0 (no scaling, byte-identical fold) unless the ramp is armed AND the host is
     * younger than `min_arc_ep + fresh_pin_ramp_ep`.  Inside the ramp the multiplier
     * decays LINEARLY from @ref fresh_pin_scale at age ≤ min_arc_ep to 1 at the ramp
     * end, so a pin never jumps discontinuously from soft to hard.
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
 * @param config_             read for the datum-mode predicates only.
 * @param log                 producer logger (debug/warn).
 * @param traces              PHASE_DATUM `trace_events` rows.
 * @param t                   this epoch's time (event stamps).
 * @param stations            [in/out] non-const: a BLOCKED key's phase-bias states are
 *                            pruned, exactly as the coverage gate prunes a thin key's.
 * @param present             this epoch's presence mask.
 * @param carry_slipped_ambs  the PASS-A slip funnel — (station index, ambiguity key)
 *                            pairs; unioned with each station's pending-reseed flags
 *                            so the age reset covers BOTH slip feeds.
 * @param cnd                 [in/out] the candidate census; blocked keys are ERASED
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

} // namespace GnssProcessor
