#pragma once
/**
 * @file NetworkAnchorReentry.hpp
 * @brief Card SID phase 4 (S2 and B1) — the ANCHOR ELECTION read against the arc
 *        ledger, and the RE-ENTRY it can then act on
 *        (@c network.ledger_screens, @c network.ledger_anchor).
 *
 * ### The defect this closes
 *
 * The election decides "is this key a first sighting or a return" by one test:
 * whether the key had an incumbent at the PREVIOUS epoch
 * (@c NetworkPhaseDatum.cpp, @c inc @c == @c anchor_incumbent.end()).  A family
 * the coverage gate dropped loses its incumbent at the drop edge and keeps
 * tracking through the gated era, so its return reads as a FIRST SIGHTING: the
 * election re-picks by coverage rank, the pin is realized cold at 0, and every
 * station's carry of the family is erased through the re-reference set.  The
 * audit named the container (`anchor_incumbent`, @c sid_audit_report_2026-09-22.md
 * E.2: "a returning key is a first sighting"), and standing invariant 7 names the
 * shape: a re-coupling edge that re-validates nothing because it cannot tell a
 * return from a birth.
 *
 * ### Two halves, two knobs
 *
 * S2 (@c network.ledger_screens) OBSERVES: for every family the election treats as
 * a first sighting, whether the ledger holds any arc of it in @c Reentered or
 * @c Gap; for every coverage-gate DROP edge, the states of the family's arcs and,
 * at the family's return, whether the returning arcs are the SAME GENERATION as
 * before the drop.  Counted and evented, nothing changed.
 *
 * B1 (@c network.ledger_anchor) ACTS on the same read: a same-generation return
 * restores the previous incumbent as the candidate of record, subject to the
 * election's existing rules.  A new-generation return proceeds as today.
 *
 * ### Why the memory lives here and not in the ledger
 *
 * The ledger owns the arc; the family's DATUM history (who anchored it, at which
 * epoch, at which generation per station) is the election's, and it is exactly the
 * state the election does not keep.  Keeping it beside the ledger rather than
 * inside it also keeps the ledger free of anything the solve can read, which is
 * the phase-1 exemption from the warm-resume fingerprint.
 */

#include "filter/ArcLedger.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief What one coverage-gate DROP EDGE left behind, for the family's return.
 *
 * The per-station ADMITTED generation is the load-bearing field: it is what makes
 * "the same arc came back" a decidable question rather than an inference from a
 * gap length.  A station whose arc died and was re-born inside the gated era has a
 * higher generation and is NOT the arc the incumbent anchored.
 */
struct AnchorDropMemo {
    std::string prev_anchor;            ///< station that anchored the family at the edge.
    long long   ep{ -1 };               ///< ledger epoch of the drop edge.
    int         n_trackers{ 0 };        ///< present stations still tracking it at the edge.
    /// station index -> the ADMITTED generation of that station's arc at the edge.
    std::map<int, int> generation;
    /// Arc states at the edge, for the S2 readout: Tracking, Gap, Dead.
    int n_tracking{ 0 }, n_gap{ 0 }, n_dead{ 0 };
    bool returned{ false };             ///< the family has already been seen to return.
};

/// @brief How a family's return classifies against its drop memo.
enum class AnchorReentryClass {
    NoMemory,        ///< no drop edge is remembered for this family.
    SameGeneration,  ///< the previous incumbent still tracks it at the SAME generation.
    NewGeneration    ///< the family returned, but not as the arcs that left.
};

/// @brief Human-readable name of a classification, for the events and the table.
const char* anchorReentryClassName(AnchorReentryClass c);

/**
 * @brief One first-sighting case the ledger says was a RETURN (S2's event row).
 */
struct AnchorFirstSighting {
    std::string amb;              ///< "<sat>_<signal>".
    std::string station;          ///< the station the election elected.
    long long   since_incumbent_ep{ -1 };  ///< epochs since the family last had an incumbent (-1 = never).
    int         n_returning{ 0 }; ///< arcs of the family in Reentered or Gap.
    int         n_tracking{ 0 };  ///< arcs of the family in Tracking.
    AnchorReentryClass cls{ AnchorReentryClass::NoMemory };
    std::string prev_anchor;      ///< the incumbent the drop memo remembers, or empty.
    bool        restored{ false };///< B1 acted and the previous incumbent was elected.
    const char* refused{ "" };    ///< why a same-generation restore did not happen.
};

/**
 * @brief Run-scope memory of the anchor read.  Lives in @c NetworkCarryState.
 *
 * Deliberately outside the warm-resume snapshot and its fingerprint: it carries
 * no value the solve reads, so a resumed run simply has no drop memo and every
 * family's first post-resume election is a genuine first sighting — the same
 * honest posture the roster and the ledger take.
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
    const ArcLedger* led{ nullptr };       ///< the ledger, READ-ONLY.
    AnchorReentryState* state{ nullptr };  ///< the run-scope memory, MUTATED.
    long long now_ep{ -1 };                ///< the ledger's own epoch index.

    // ---- outputs, rebuilt every epoch ------------------------------------
    std::vector<AnchorFirstSighting> first_sightings;  ///< S2 rows for this epoch.
    long long n_first_sighting{ 0 };        ///< families elected with no incumbent.
    long long n_first_sighting_return{ 0 }; ///< of those, whose arcs had just returned.
    long long n_restore{ 0 };               ///< B1: the previous incumbent was restored.
    long long n_reentry_new{ 0 };           ///< B1: the return was NEW generations.
    long long n_restore_refused{ 0 };       ///< same generation, but the station could not be elected.
};

/**
 * @brief Classify one family's return against its drop memo.  PURE.
 *
 * The rule, stated once so the unit suite can pin it and the election cannot
 * drift from it:
 *
 *  - no memo, or a memo with no remembered incumbent, is @c NoMemory;
 *  - the memo's incumbent station must still hold an arc of this family whose
 *    ADMITTED state is alive (@c Tracking or @c Reentered) and whose
 *    @c adm_generation equals the generation recorded at the drop edge: that is
 *    @c SameGeneration, the one case B1 acts on;
 *  - anything else is @c NewGeneration.
 *
 * The generation is the ADMITTED one because the carried ambiguity, the leveling
 * level and the phase-bias column are all admitted-side objects: a raw-side
 * re-birth the estimator never modelled does not change what the incumbent's
 * carry means.
 *
 * @param led        The ledger, READ-ONLY.
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
 * @brief Census of one family's arc states in the ledger.  PURE.
 *
 * @param led    The ledger, READ-ONLY.
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
 * @brief Record one coverage-gate DROP EDGE into the run-scope memory.
 *
 * Called from the election's own drop-edge block, where the pre-swap incumbent
 * map still describes the previous epoch, which is the only place the family's
 * outgoing anchor is knowable.
 *
 * @param st          The memory, MUTATED.
 * @param led         The ledger, READ-ONLY.
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
 * @brief The ADMITTED age of one arc, in epochs: the ledger's answer to the
 *        question @c anchor_arc_age asks.  PURE.
 *
 * Age is @c ep @c - @c ep_adm_state_changed on the admitted side, which is the
 * disposition the audit gave the container (E.2, "age is ep - ep_state_changed").
 * An arc the ledger has never admitted has no age and the function returns -1, so
 * the caller can keep its own seed rather than inventing one.
 *
 * @param led    The ledger, READ-ONLY.
 * @param key    The arc.
 * @param now_ep The ledger's own epoch index.
 */
long long anchorLedgerArcAge(const ArcLedger& led, const ArcLedger::Key& key,
                             long long now_ep);

} // namespace GnssProcessor
