#pragma once
/**
 * @file NetworkAnchorReentry.hpp
 * @brief Anchor election read against the arc ledger, and the re-entry it can act on
 *        (@c network.ledger_screens, @c network.ledger_anchor; card SID phase 4,
 *        S2 and B1).
 *
 * ### First sighting versus return
 *
 * With both knobs off, the election decides whether a key is a first sighting or a
 * return by one test: whether the key had an incumbent at the previous epoch
 * (@c NetworkPhaseDatum.cpp, @c inc @c == @c anchor_incumbent.end()).  A family
 * that the coverage gate dropped loses its incumbent at the drop edge but keeps
 * tracking through the gated era, so its return reads as a first sighting: the
 * election re-picks by coverage rank, the pin is realized cold at 0, and every
 * station's carry of the family is erased through the re-reference set.  The audit
 * identifies the container (`anchor_incumbent`, @c sid_audit_report_2026-09-22.md
 * E.2: "a returning key is a first sighting").  This is the re-coupling edge of
 * invariant #7 with no re-validation, because it cannot tell a return from a birth.
 *
 * ### Two halves, two knobs
 *
 * S2 (@c network.ledger_screens) observes: for every family the election treats as
 * a first sighting, whether the ledger holds any arc of it in @c Reentered or
 * @c Gap; for every coverage-gate drop edge, the states of the family's arcs and,
 * at the family's return, whether the returning arcs are the same generation as
 * before the drop.  It counts and emits events and changes nothing.
 *
 * B1 (@c network.ledger_anchor) acts on the same read: a same-generation return
 * restores the previous incumbent as the candidate of record, subject to the
 * election's existing rules.  A new-generation return goes through the unchanged
 * election.
 *
 * ### Why the memory lives here and not in the ledger
 *
 * The ledger owns the arc; the family's datum history (which station anchored it,
 * at which epoch, at which generation per station) belongs to the election, which
 * does not otherwise keep it.  Keeping it beside the ledger rather than inside it
 * also keeps the ledger free of anything the solve can read, which is the basis of
 * the ledger's phase-1 exemption from the warm-resume fingerprint.
 */

#include "filter/ArcLedger.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief What one coverage-gate drop edge left behind, for the family's return.
 *
 * The per-station admitted generation is the field the classification depends on:
 * it makes "the same arc came back" a decidable question rather than an inference
 * from a gap length.  A station whose arc died and was re-born inside the gated era
 * has a higher generation and is not the arc the incumbent anchored.
 */
struct AnchorDropMemo {
    std::string prev_anchor;            ///< station that anchored the family at the edge.
    long long   ep{ -1 };               ///< ledger epoch of the drop edge.
    int         n_trackers{ 0 };        ///< present stations still tracking it at the edge.
    /// station index -> the admitted generation of that station's arc at the edge
    /// (admitted-alive arcs only: Tracking or Reentered).
    std::map<int, int> generation;
    /// Arc states at the edge, for the S2 readout: Tracking, Gap (includes Reentered), Dead.
    int n_tracking{ 0 }, n_gap{ 0 }, n_dead{ 0 };
    bool returned{ false };             ///< the family has already been seen to return.
};

/// @brief How a family's return classifies against its drop memo.
enum class AnchorReentryClass {
    NoMemory,        ///< no drop edge is remembered for this family.
    SameGeneration,  ///< the previous incumbent still tracks it at the generation recorded at the drop.
    NewGeneration    ///< the family returned, but not as the arcs that left.
};

/// @brief Human-readable name of a classification, for the events and the table.
const char* anchorReentryClassName(AnchorReentryClass c);

/**
 * @brief One first-sighting case that the ledger shows to be a return (S2's event row).
 */
struct AnchorFirstSighting {
    std::string amb;              ///< "<sat>_<signal>".
    std::string station;          ///< the station the election elected.
    long long   since_incumbent_ep{ -1 };  ///< epochs since the family last had an incumbent (-1 = never).
    int         n_returning{ 0 }; ///< arcs of the family in Reentered or Gap.
    int         n_tracking{ 0 };  ///< arcs of the family in Tracking.
    AnchorReentryClass cls{ AnchorReentryClass::NoMemory };
    std::string prev_anchor;      ///< the incumbent the drop memo remembers, or empty.
    bool        restored{ false };///< set when B1 acted and the previous incumbent was elected.
    const char* refused{ "" };    ///< why a same-generation restore did not happen.
};

/**
 * @brief Run-scope memory of the anchor read.  Lives in @c NetworkCarryState.
 *
 * Kept outside the warm-resume snapshot and its fingerprint: it carries no value the
 * solve reads, so a resumed run has no drop memo and every family's first
 * post-resume election is treated as a first sighting, as the roster and the
 * ledger also do after a resume.
 */
struct AnchorReentryState {
    /// "<sat>_<signal>" -> what its last coverage-gate drop edge left behind.
    std::map<std::string, AnchorDropMemo> drop;
    /// "<sat>_<signal>" -> the last ledger epoch at which the family had an incumbent.
    std::map<std::string, long long> last_incumbent_ep;
};

/**
 * @brief The per-epoch view handed to the election.  Null on every unarmed run.
 *
 * @c observe alone is the S2 observatory (byte-identical); @c restore adds B1's
 * action.  @c restore without @c observe is refused by the loader, so the two are
 * never out of step.
 */
struct AnchorReentry {
    bool observe{ false };                 ///< network.ledger_screens.
    bool restore{ false };                 ///< network.ledger_anchor.
    const ArcLedger* led{ nullptr };       ///< the ledger (read-only).
    AnchorReentryState* state{ nullptr };  ///< the run-scope memory (mutated).
    long long now_ep{ -1 };                ///< the ledger's own epoch index.

    // ---- outputs, rebuilt every epoch ------------------------------------
    std::vector<AnchorFirstSighting> first_sightings;  ///< rows for this epoch (S2).
    long long n_first_sighting{ 0 };        ///< families elected with no incumbent.
    long long n_first_sighting_return{ 0 }; ///< of those, whose arcs had just returned.
    long long n_restore{ 0 };               ///< returns where the previous incumbent was restored (B1).
    long long n_reentry_new{ 0 };           ///< returns classified as new generations (B1).
    long long n_restore_refused{ 0 };       ///< same generation, but the station could not be elected.
};

/**
 * @brief Classify one family's return against its drop memo.  Pure function.
 *
 * The rule is stated once here so the unit suite can pin it and the election
 * cannot diverge from it:
 *
 *  - no memo, or a memo with no remembered incumbent, is @c NoMemory;
 *  - the memo's incumbent station must still hold an arc of this family whose
 *    admitted state is alive (@c Tracking or @c Reentered) and whose
 *    @c adm_generation equals the generation recorded at the drop edge: that is
 *    @c SameGeneration, the one case B1 acts on;
 *  - anything else is @c NewGeneration.
 *
 * The generation is the admitted one because the carried ambiguity, the leveling
 * level and the phase-bias column are all admitted-side objects: a raw-side
 * re-birth the estimator never modelled does not change what the incumbent's
 * carry means.
 *
 * @param led        The ledger (read-only).
 * @param memo       The family's drop memo.
 * @param sat        Satellite of the family.
 * @param signal     Signal of the family.
 * @param sta_index  Station name -> index, for resolving the memo's incumbent.
 * @param out_station Optional: receives the incumbent's station index on
 *                    @c SameGeneration, or -1.
 */
AnchorReentryClass anchorReentryClassify(const ArcLedger& led,
                                         const AnchorDropMemo& memo,
                                         const std::string& sat,
                                         const std::string& signal,
                                         const std::map<std::string, int>& sta_index,
                                         int* out_station = nullptr);

/**
 * @brief Census of one family's arc states in the ledger (admitted side).  Pure
 *        function.
 *
 * @param led    The ledger (read-only).
 * @param sat    Satellite of the family.
 * @param signal Signal of the family.
 * @param n_tracking Receives arcs in @c Tracking.
 * @param n_gap      Receives arcs in @c Gap or @c Reentered.
 * @param n_dead     Receives arcs in @c Dead.
 * @param gen        Optional: receives station index -> admitted generation for
 *                   every arc that is admitted-alive.
 */
void anchorReentryFamilyCensus(const ArcLedger& led, const std::string& sat,
                               const std::string& signal,
                               int& n_tracking, int& n_gap, int& n_dead,
                               std::map<int, int>* gen = nullptr);

/**
 * @brief Record one coverage-gate drop edge into the run-scope memory.
 *
 * Called from the election's own drop-edge block, where the pre-swap incumbent
 * map still describes the previous epoch; that is the only place the family's
 * outgoing anchor is known.  A later drop of the same family replaces the memo.
 *
 * @param st          The memory (mutated).
 * @param led         The ledger (read-only).
 * @param amb         "<sat>_<signal>".
 * @param sat         Satellite of the family.
 * @param signal      Signal of the family.
 * @param prev_anchor The station that anchored it at the previous epoch.
 * @param n_trackers  Present stations still tracking it.
 * @param now_ep      The ledger's own epoch index.
 */
void anchorReentryNoteDrop(AnchorReentryState& st, const ArcLedger& led,
                           const std::string& amb, const std::string& sat,
                           const std::string& signal,
                           const std::string& prev_anchor, int n_trackers,
                           long long now_ep);

/**
 * @brief The admitted age of one arc, in epochs: the ledger's answer to the
 *        question @c anchor_arc_age asks.  Pure function.
 *
 * Age is @c ep @c - @c ep_adm_state_changed on the admitted side, which is the
 * disposition the audit gave the container (E.2, "age is ep - ep_state_changed").
 * The function returns -1 when the arc is not in the ledger, when its admitted
 * state is not alive (@c Tracking or @c Reentered), when it has no admitted state
 * change recorded, or when the age would be negative; the caller then keeps its own
 * seed rather than inventing one.
 *
 * @param led    The ledger (read-only).
 * @param key    The arc.
 * @param now_ep The ledger's own epoch index.
 */
long long anchorLedgerArcAge(const ArcLedger& led, const ArcLedger::Key& key,
                             long long now_ep);

} // namespace GnssProcessor
