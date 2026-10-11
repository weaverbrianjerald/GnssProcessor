#pragma once
/**
 * @file NetworkResponses.hpp
 * @brief The responses of the network's carried parameters, declared and counted (card PRM
 *        phase 3a, docs/refound/PRM_parameter_registry_2026-09-29.md section 4.5).
 *
 * A carried instance (a border entry of @c prev_carry, a station-local carry) responds to
 * events: a slip, a gap, a datum handover, a decouple, a re-admission.  Each response was
 * written where its mechanism needed it, and the run end reported some of them, under the
 * mechanism's own words, and not others: card RF-1's design section 1.5 names four paths
 * for one ambiguity and counts two of them only in aggregate.  This header declares every
 * site that changes a carried instance in response to an event, one row per (kind, event,
 * policy, site) (@ref ResponseSite), with the condition under which the site can act on
 * the run (@ref responseSiteArmed) and the count of instances it changed
 * (@c NetworkServiceCounters::prm_resp, or the fold's disposition tally where the fold
 * already counts the response).  The registry reads the table to declare each kind's
 * policies per event (@c ParamSpec::on), and the run end writes the table with its counts
 * and a check: a site that responded on a run where it is not declared armed is a
 * declaration error, warned at the run end.
 *
 * The declared datum moves of card PRM phase 2 (@ref CarryMechanism) are the Stransform
 * responses; their counts are the traversal's moved values per kind
 * (@ref carryMechanismArmed declares when each can act).
 *
 * Phase 3a changes no number: the counters are increments beside the existing actions.
 * Phase 3b gives a policy one implementation where the sites already agree, byte for byte:
 * the gap test (@ref carryPastHorizon) and the gap prune (@ref carryGapPrune), against the
 * horizon the registry declares per kind (@c ParamSpec::gap_horizon_s).
 * Not responses, and not in the table: the q rule's time update (the registry's q), the
 * numerical guards that decorrelate or skip a carried prior that fails a positive-definite
 * or finite screen, the coupling terms' re-attribution to a border key's disposition (card
 * CTU's run-end line), and the marks that only re-seed a linearization point (a
 * pending-reseed mark consumed by the measurement pre-pass, a solve no-op).
 */

#include "filter/NetworkCarrySites.hpp"     // CarryMechanism
#include "filter/NetworkParamRegistry.hpp"  // NetworkParamKind, ParamEvent, ResponsePolicy

#include <cstdint>

namespace GnssProcessor {

class  GnssConfig;               // config/GnssConfig.hpp
struct NetworkKnobs;             // app/NetworkRunContext.hpp
struct NetworkServiceCounters;   // app/NetworkRunContext.hpp
struct NetworkRunContext;        // app/NetworkRunContext.hpp

/**
 * @brief Every site at which a carried instance responds to an event, one row per
 *        (kind, event, policy, code site).
 *
 * The names are the run-end table's; @ref responseSiteSpec gives the kind, event, policy,
 * the code site and what one count is.
 */
enum class ResponseSite : int {
    // --- Ambiguity (local_carry) -------------------------------------------------------
    AmbDropLatchSlip = 0,  ///< float vehicle: a latched arc dropped, latched by anything but a re-entry response alone (MW, LLI, IF-TD, the event harness's T3/T4 tag, or no recorded cause)
    AmbDropLatchReentry,   ///< float vehicle: a latched arc dropped, latched only by a re-entry response (gap re-entry, wind-up restart)
    AmbDropStaleGap,       ///< float vehicle: an arc past the gap horizon dropped
    AmbFreeEpochLatch,     ///< leveling vehicle: a latched arc not folded this epoch, latched by anything but a re-entry response alone
    AmbFreeEpochLatchReentry, ///< leveling vehicle: a latched arc not folded this epoch, latched only by a re-entry response
    AmbPendingReseedSkip,  ///< an arc with a pending-reseed mark at the inject not folded this epoch
    AmbT2ReplayFree,       ///< the within-epoch replay: an arc's prior withheld for the replay
    AmbGapSkip,            ///< leveling vehicle: an arc past the gap horizon not folded this epoch
    AmbGapPrune,           ///< an arc unseen past the gap horizon erased
    AmbRaimDrop,           ///< float vehicle: RAIM's attribution or consensus dropped a carried arc
    AmbRaimErase,          ///< leveling vehicle: RAIM's attribution or consensus erased a carried arc
    AmbRaimRetap,          ///< RAIM's attribution kept the value and inflated the variance
    AmbDatumSlipErase,     ///< a slip at the datum station re-referenced the family: every station's arc erased
    AmbGrossErase,         ///< the gross line-of-sight screen erased a carried arc
    AmbGrossHold,          ///< the gross line-of-sight screen held a carried arc
    AmbWithholdErase,      ///< a withheld epoch's free response erased a carried arc
    AmbAnchorRerefErase,   ///< a phase-anchor re-reference (handover, coverage drop) erased the family's arcs
    AmbEq34Erase,          ///< the eq34 chain's re-reference erased a carried arc
    AmbFleetFlush,         ///< the SH field's convergence flushed every carried arc
    // --- Slant ionosphere (iono_carry, iono_blk_carry) ---------------------------------
    IonoSlipErase,         ///< a slip on the line of sight erased its carried ionosphere
    IonoRaimErase,         ///< RAIM's attribution erased the line of sight's carried ionosphere
    IonoGrossErase,        ///< the gross line-of-sight screen erased the carried ionosphere
    IonoGrossHold,         ///< the gross line-of-sight screen held the carried ionosphere
    IonoGapSkip,           ///< a carried ionosphere past its horizon not folded
    IonoGapPrune,          ///< a carried ionosphere unseen past its horizon erased
    // --- Phase colour (pc_carry, card LGM) ---------------------------------------------
    PcRaimErase,           ///< RAIM's attribution erased the line of sight's phase colour
    PcGrossErase,          ///< the gross line-of-sight screen erased the phase colour
    PcGapPrune,            ///< a phase colour unseen past its horizon erased
    // --- Receiver code bias (rx_cb_carry) ----------------------------------------------
    RxcbStepErase,         ///< the harvest's step test erased a carried receiver code bias
    RxcbPeerStepErase,     ///< the peer-relative step gate erased a carried receiver code bias
    RxcbJumpSkip,          ///< the diagonal route's innovation test did not fold a receiver code bias
    RxcbGapSkip,           ///< a receiver code bias past the gap horizon not folded (either route)
    RxcbGapPrune,          ///< a receiver code bias unseen past the horizon erased
    // --- Receiver clock (rx_clk_carry) -------------------------------------------------
    RxclkJumpErase,        ///< the one-step prediction test erased a carried receiver clock
    RxclkGapSkip,          ///< a receiver clock past the gap horizon not folded
    RxclkGapPrune,         ///< a receiver clock unseen past the horizon erased
    // --- Receiver phase bias (rpb_carry) -----------------------------------------------
    RpbEq34Erase,          ///< the eq34 chain's re-reference erased a receiver phase bias
    RpbGapSkip,            ///< a receiver phase bias past the gap horizon not folded
    RpbGapPrune,           ///< a receiver phase bias unseen past the horizon erased
    // --- Wet delay (zwd_carry) ---------------------------------------------------------
    ZwdGapSkip,            ///< a wet delay past the gap horizon not folded
    ZwdGapPrune,           ///< a wet delay unseen past the horizon erased
    // --- Satellite clock (prev_carry, clock_osc) ---------------------------------------
    ClkDeparted,           ///< folded with its column departed (marginalized)
    ClkPark,               ///< parked at its column's departure (READMIT)
    ClkParkExpire,         ///< a parked clock past the seed horizon dropped
    ClkSeed,               ///< a parked clock seeded its returned column
    ClkParkDiscard,        ///< a parked clock whose column returned without consuming it dropped
    ClkParkDiscardDatum,   ///< a parked clock dropped at a clock-datum change
    ClkReinit,             ///< folded at the clock monitor's reinit variance
    ClkRetire,             ///< not folded: a clock-pivot handover retired its datum
    ClkAnchorDecouple,     ///< card Q3's guard decoupled the clock from its anchor (folds loose)
    ClkAnchorRecouple,     ///< card Q3's guard re-validated the clock (folds anchored again)
    ClkOscWipe,            ///< the clock oscillator's state wiped at a clock-datum change
    ClkOscReinit,          ///< the clock oscillator re-initialized on a flagged fault
    ClkOscGapReset,        ///< the clock oscillator's drift reset past its maximum gap
    // --- Satellite code bias (prev_carry, the GF time link) ----------------------------
    CbDeparted,            ///< folded with its column departed (marginalized)
    CbPark,                ///< parked at its column's departure (READMIT)
    CbParkExpire,          ///< a parked code bias past the seed horizon dropped
    CbSeed,                ///< a parked code bias seeded its returned column
    CbParkDiscard,         ///< a parked code bias whose column returned without consuming it dropped
    CbParkDiscardDatum,    ///< a parked code bias dropped at a clock-datum change
    CbReinit,              ///< folded at the reinit variance
    CbRetire,              ///< not folded: a clock-pivot handover retired its datum
    CbTimeLinkReanchor,    ///< the geometry-free time link re-anchored at its target on a return
    // --- Satellite phase bias (prev_carry, pin_bridge) ---------------------------------
    PbDeparted,            ///< folded with its column departed (marginalized)
    PbReref,               ///< not folded: its frame was re-referenced this epoch
    PbReinit,              ///< folded at the reinit variance
    PbRetire,              ///< not folded: a clock-pivot handover retired its datum
    PbPinBridgeDiscard,    ///< an open pin bridge's held value dropped at a datum change
    // --- SH coefficient (prev_carry) ---------------------------------------------------
    ShRetire,              ///< a carried SH entry not folded at a clock-datum change
};

/// Number of @ref ResponseSite values (array sizing).
inline constexpr int kResponseSites = 69;

static_assert(static_cast<int>(ResponseSite::ShRetire) + 1 == kResponseSites,
              "kResponseSites must follow the ResponseSite enum");

/// Where a site's count comes from.
enum class ResponseCountSource : int {
    Increment = 0,   ///< @c NetworkServiceCounters::prm_resp, incremented at the site
    Disposition,     ///< the fold's tally @c prm_disp[kind][disposition] (key-epochs)
    Departed,        ///< the fold's tally @c prm_departed[kind] (key-epochs)
};

/**
 * @brief The declaration of one response site: a row of the run-end response table.
 */
struct ResponseSiteSpec {
    ResponseSite        site;
    const char*         name;         ///< short name (the run-end table, the manifest)
    NetworkParamKind    kind;         ///< the kind whose carried instance it changes
    ParamEvent          event;        ///< the event it responds to
    ResponsePolicy      policy;       ///< what it does
    ResponseCountSource source;       ///< where the count comes from
    int                 disposition;  ///< the @c CarryDisposition of a Disposition source; -1 otherwise
    const char*         where;        ///< the code site (file and function)
    const char*         unit;         ///< what one count is
};

/// The declaration of @p s.
const ResponseSiteSpec& responseSiteSpec(ResponseSite s);

/**
 * @brief Whether site @p s can act on a run with knob snapshot @p k and configuration
 *        @p config.
 *
 * The site's own gate as the code reads it (the vehicle, the knob that arms the
 * mechanism, the route), from the knob snapshot where the snapshot carries it and from the
 * configuration otherwise.  A site that is armed may still never fire (no event on the
 * run); a site that fires on a run where it is not armed is a declaration error, which the
 * run end warns about.
 */
bool responseSiteArmed(ResponseSite s, const NetworkKnobs& k, const GnssConfig& config);

/**
 * @brief Whether the datum mechanism @p m can act on a run: the Stransform declaration of
 *        the kinds whose carried values it moves.
 */
bool carryMechanismArmed(CarryMechanism m, const NetworkKnobs& k, const GnssConfig& config);

/**
 * @brief The border kinds whose carried @c prev_carry entries mechanism @p m moves
 *        (a mask of @c 1u << SharedParamKind).
 */
std::uint32_t carryMechanismBorderKinds(CarryMechanism m, const NetworkKnobs& k);

/**
 * @brief The station-local kind whose carried value site @p s holds, if @p s is a
 *        local value site (an ambiguity, a receiver clock, a receiver phase or code bias);
 *        false otherwise (a regression term, a baseline or a pin target is a reference, not
 *        a carried instance).
 */
bool carrySiteLocalKind(CarrySite s, NetworkParamKind& kind);

/// Add @p n responses at site @p s to a run-scope counter array.
inline void prmRespond(long long (&counts)[kResponseSites], ResponseSite s, long long n = 1) {
    counts[static_cast<int>(s)] += n;
}

/**
 * @brief Whether a carried instance last seen at @p last_seen_s is past its gap horizon at
 *        @p now_s: the one gap test of the GapSkip and GapPrune policies (card PRM phase 3b).
 *
 * Exactly the comparison the gap sites made before phase 3b, operand for operand: the gap
 * @c now_s - @c last_seen_s, then a strict @c > against the horizon.  A gap equal to the
 * horizon is inside it.  There is no guard on a negative or non-finite gap: a negative gap
 * is never past a non-negative horizon, and a NaN gap or a NaN horizon compares false, so
 * the test never fires on one.  A site whose fold test also refuses a negative gap (the
 * receiver phase bias's time link) keeps that guard on its fold branch.
 *
 * @param now_s       The current epoch time (s; the harvest's or the inject's @c pl_now_s).
 * @param last_seen_s The instance's stash time (s; @c LocalCarry::last_seen_s).
 * @param horizon_s   The kind's gap horizon (s; @c ParamSpec::gap_horizon_s, or the knob
 *                    the site reads).
 * @return @c true when the gap exceeds the horizon.
 */
inline bool carryPastHorizon(double now_s, double last_seen_s, double horizon_s) {
    return now_s - last_seen_s > horizon_s;
}

/**
 * @brief The GapPrune policy, one implementation (card PRM phase 3b): erase every carried
 *        instance of @p m that is past the horizon.
 *
 * Walks @p m from @c begin() to @c end() in the map's order.  For an entry that is
 * @ref carryPastHorizon at @p now_s it calls @p on_erase with the entry's iterator, then
 * erases the entry; any other entry is kept.  @p on_erase carries what the site does
 * beside the erase (its counters, its traces, the erase of a companion map's entry), and
 * runs while the entry is still in the map, so it may read the key and the value.  It must
 * not insert into or erase from @p m.
 *
 * @tparam Map     An associative container whose @c erase(iterator) returns the next
 *                 iterator and whose mapped type has a @c last_seen_s member (s), e.g.
 *                 @c std::map<Key, LocalCarry>.
 * @tparam OnErase Callable as @c on_erase(const typename Map::iterator&).
 * @param m         The carry map.
 * @param now_s     The current epoch time (s).
 * @param horizon_s The kind's gap horizon (s).
 * @param on_erase  Called once per pruned entry, before its erase.
 * @return The number of entries erased.
 */
template <class Map, class OnErase>
long long carryGapPrune(Map& m, double now_s, double horizon_s, OnErase on_erase) {
    long long n = 0;
    for (auto it = m.begin(); it != m.end();) {
        if (carryPastHorizon(now_s, it->second.last_seen_s, horizon_s)) {
            const auto& entry = it;   // the callback reads the entry; the walk owns the cursor
            on_erase(entry);
            it = m.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

/**
 * @brief The free-epoch latch's causes, counted (card RF-1 section 1.5: "free-epoch latches
 *        not individually traced").
 *
 * One row per cause bit of @c RelevelEpochState::free_cause, plus the latched keys with no
 * cause recorded: the (station, ambiguity) keys latched this epoch with the bit set, and of
 * those the ones that held a carried entry at the response (the float vehicle's drop pass;
 * the leveling vehicle's inject reads the same set).
 */
inline constexpr int kLatchCauses = 9;   ///< eight cause bits and "none"

/// Short name of latch cause row @p i (bit order of @c RelevelEpochState, then "none").
const char* latchCauseName(int i);

/**
 * @brief The @c prm_disp / @c prm_departed index (@c SharedParamKind) of a border kind.
 * @return the index, or -1 for a station-local kind.
 */
int responseBorderIndex(NetworkParamKind kind);

/**
 * @brief The count of site @p s over the run, from its declared source.
 * @param C the run's counters
 * @param s the site
 */
long long responseSiteCount(const NetworkServiceCounters& C, ResponseSite s);

/**
 * @brief Count this epoch's free-epoch latch per cause (@c prm_latch_*), measure-only.
 *
 * Called once per epoch after PASS A's slip scans have latched the set and before the
 * response reads it (the float vehicle's drop pass; the leveling vehicle's inject), so a
 * key's "carried" is whether the response found an entry to act on.
 * @param ctx run context (the counters are written; nothing else)
 */
void networkResponseLatchTally(NetworkRunContext& ctx);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the declared responses of the carried instances (PRM 3a).
 *
 * The declared responses of the carried instances (card PRM phase 3a), with the
 * `param_responses` manifest record.
 *
 * Gate: prints always (a block).
 *
 * @param v The run at its end (filter/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkResponsesRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

}  // namespace GnssProcessor
