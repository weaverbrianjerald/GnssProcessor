#pragma once
/**
 * @file NetworkBiasAdmission.hpp
 * @brief Run-latched startup census for the PPP-RTK network producer's W6
 *        satellite code-bias (OSB) columns — admission vs station-local
 *        quarantine (the "CB-ADM" card).
 *
 * The legacy W6 gate (@ref Application::networkAssemblyRegisterColumns_) admits
 * a per-(sat,signal) code-bias column whenever >= 2 PRESENT stations hold its
 * sat-bias state THIS epoch, and prunes everything else back to a station-local
 * nuisance.  At BeiDou scale a large fraction of (sat,signal) pairs sit at 1-2
 * stations and FLAP across that instantaneous threshold — alternating epochs of
 * {registered + datumed} vs {pruned + leaking}, with prune / warm-restore and
 * Card-2 datum re-seed churn, and datum-orphan near-null directions that
 * withhold the WHOLE epoch through the null-space rank gate.
 *
 * This seam replaces the per-epoch count with a ONE-SHOT census over the first
 * K epochs the run loop will process.  Each (sat,signal) code pair is latched
 * once, immutable for the run, into two classes:
 *
 *   - ADMITTED     — a shared border column, datumed and emitted, exactly as a
 *                    registered pair is today.  Admitted whenever >= 1 present
 *                    station holds its state (rides straight through 1-station
 *                    dips — the anti-flap win), never per-epoch re-evaluated.
 *   - QUARANTINED  — measurements kept; the per-station SM sat-bias state kept
 *                    and NEVER pruned while the flag is on; never registered ⇒
 *                    a station-local sponge that is Schur-eliminated station-
 *                    side and never enters the shared normals, spectrum, or
 *                    rank gate.  This is the DEFAULT class (pairs first seen
 *                    after the census window quarantine automatically).
 *
 * Two admission tiers (see @ref runCensus):
 *   - tier 1 (count):  a station SUPPORTS a pair iff it supplies a usable raw
 *                      code observation in >= ceil(K_eff/2) census epochs (an
 *                      absolute sustained-support floor, not a flimsy 2-of-2
 *                      ratio); admitted-candidate iff #supporting stations >=
 *                      effective_min = max(2, min(min_stations, n_census_sta)).
 *   - tier 2 (IF-pin): per satellite, its tier-1 candidates must span >= 2
 *                      DISTINCT carrier bands (the same distinctness the clock
 *                      IF pin needs) — else ALL of that satellite's candidates
 *                      are demoted to QUARANTINED, killing the datum-orphan
 *                      near-null class at the source rather than at the gate.
 *
 * This class owns NO run state and introduces NO stochastic machinery:
 * quarantined states inherit today's birth seed, initial sigma, sat_code_bias_q
 * process noise, and warm cache untouched.  @ref runCensus is a pure scan over
 * the already-in-memory synced-epoch vector; the result is a POD latched once
 * into @ref NetworkRunContext and read (as a membership test) at the
 * registration site.  Default-OFF; when the flag is off the census never runs
 * (@c active stays false) and the legacy per-epoch path is byte-identical.
 *
 * Seeded at the CB-ADM card (2026-07-21); sibling of @ref NetworkSbasis /
 * @ref NetworkPhaseDatum.  See docs/SPEC_cb_admission_card.md.
 */

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

struct SyncedEpoch;  ///< rinex/MultiRinexSync.hpp — the census input (fwd-decl keeps this header light).

/**
 * @brief Result of the run-latched W6 code-bias admission census: the admitted
 *        (sat,signal) key set plus per-pair verdicts and counters for logging.
 *
 * Default-constructed with @c active==false (the OFF / census-never-ran state),
 * so a NetworkRunContext holding one by value costs nothing until @ref runCensus
 * fills it.  The admitted set uses the SAME key format as the StateManager
 * sat-bias states — @c "satellite|signal" (@ref StateKeys::makeSatBiasKey) — so
 * the registration site can test membership directly against @c
 * getSatBiasStates() keys.
 */
struct NetworkBiasAdmission {
    /// @brief Per-pair census verdict.
    enum class Verdict {
        Admitted,   ///< Shared border column (datumed + emitted) — passed both tiers.
        LowCount,   ///< Quarantined: fewer than @c effective_min supporting stations (tier 1).
        NoIfPin,    ///< Quarantined: satellite's tier-1 candidates span < 2 distinct bands (tier 2).
    };

    /// @brief One (sat,signal) code pair observed during the census window.
    struct Pair {
        std::string key;                     ///< "satellite|signal" (== the StateManager sat-bias key).
        std::string satellite;               ///< RINEX satellite id (e.g. "C21").
        std::string signal;                  ///< RINEX code obs id (e.g. "C2I").
        int         support{0};              ///< # stations supporting the pair (tier-1 sustained-support count).
        Verdict     verdict{Verdict::LowCount};
    };

    std::set<std::string> admitted;          ///< Datum-grade "sat|sig" keys (fast membership test at the reg site).
    std::set<std::string> seen;              ///< Every "sat|sig" key the census observed (admitted ∪ census-quarantined); "was this classified?".
    std::vector<Pair>     census;            ///< Every pair seen in the window (deterministic key order) — logging/telemetry.
    std::set<std::string> late_tracked;      ///< RUN-MUTABLE (not a census output): distinct keys first seen at the reg site post-census — dedups n_cb_late_quarantined.
    int  n_admitted{0};                      ///< == admitted.size().
    int  n_quarantined_census{0};            ///< census.size() - n_admitted (distinct pairs quarantined at census close).
    int  census_epochs_used{0};              ///< K_eff actually scanned (clamped to the available span).
    int  census_stations{0};                 ///< Distinct stations appearing anywhere in the census window.
    int  effective_min{0};                   ///< max(2, min(min_stations, census_stations)) — the tier-1 station threshold.
    int  sustained_floor{0};                 ///< ceil(K_eff/2) — per-station epoch count a station needs to "support" a pair.
    bool active{false};                      ///< Flag armed AND the census actually classified >= 1 epoch.

    /// @brief True iff @p key ("satellite|signal") was admitted as a shared column.
    bool isAdmitted(const std::string& key) const { return admitted.count(key) != 0; }

    /**
     * @brief Run the one-shot admission census over the census window
     *        @c [i0, min(i0+census_epochs, synced.size())).
     *
     * Pure scan — reads @p synced, allocates nothing outside the returned POD,
     * and is fully deterministic (std::map/std::set iteration order) so the
     * classification is reproducible run-to-run.  The usable-observation
     * predicate over @c SyncedEpoch::by_station -> @c RinexObservation::
     * measurements is: 3-char id, @c id[0]=='C' (code obs), no '_', finite
     * nonzero value; GLONASS satellites (@c satellite[0]=='R') are skipped (no
     * OSB scope).  Distinctness for tier 2 uses @ref classifySignalForSat's
     * @c band (two codes are distinct iff their carrier bands differ — the same
     * criterion @ref NetworkSbasis::selectIfPinPair applies via carrier
     * frequency; a same-band co-channel like C1P vs C1X is NOT distinct).
     *
     * @param synced        The whole in-memory synced-epoch vector.
     * @param i0            Index of the FIRST epoch the run loop will process
     *                      (post @c start_offset_hours fast-forward) — the
     *                      census must classify on data the loop actually sees.
     * @param census_epochs Requested window length K (@c cb_admission_census_epochs).
     *                      Clamped to the available span (K_eff <= K).
     * @param min_stations  Requested minimum supporting-station count
     *                      (@c cb_admission_min_stations); folded into
     *                      @c effective_min = max(2, min(min_stations, n_census_sta)).
     * @return A latched result; @c active==false (legacy fallback) when the
     *         window is empty (synced empty, @p i0 past the end, or
     *         @p census_epochs <= 0).
     */
    static NetworkBiasAdmission runCensus(const std::vector<SyncedEpoch>& synced,
                                          std::size_t i0,
                                          int census_epochs,
                                          int min_stations);
};

} // namespace GnssProcessor
