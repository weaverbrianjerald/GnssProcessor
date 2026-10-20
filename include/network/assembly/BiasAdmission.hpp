#pragma once
/**
 * @file BiasAdmission.hpp
 * @brief Run-latched startup census for the PPP-RTK network producer's W6
 *        satellite code-bias (OSB) columns: admission versus station-local
 *        quarantine.
 *
 * The legacy W6 gate (@ref Application::networkAssemblyRegisterColumns_) admits
 * a per-(sat,signal) code-bias column whenever at least 2 present stations hold
 * its sat-bias state in the current epoch, and prunes every other pair back to a
 * station-local nuisance.  With BeiDou many (sat,signal) pairs are held by only
 * 1–2 stations and cross that instantaneous threshold back and forth.  The pair
 * then alternates between registered and datumed, and pruned and leaking, with
 * prune / warm-restore and code-bias time-link datum re-seed churn; and a datum-orphan
 * near-null direction withholds the whole epoch through the null-space rank gate.
 *
 * This seam replaces the per-epoch count with a one-shot census over the first
 * K epochs the run loop will process.  Each (sat,signal) code pair is latched
 * once, immutable for the run, into one of two classes:
 *
 *   - Admitted:    a shared border column, datumed and emitted like a pair
 *                  registered by the legacy gate.  The verdict is never
 *                  re-evaluated.  In a given epoch the column registers when at
 *                  least 2 present stations hold its state; below that it is
 *                  withheld for the epoch, but its station states are kept, so
 *                  it registers again when coverage returns (no flapping
 *                  through prune and re-seed).
 *   - Quarantined: measurements kept; the per-station SM sat-bias state kept and
 *                  never pruned while the flag is on; never registered.  The
 *                  state is a station-local nuisance that is Schur-eliminated
 *                  station-side and never enters the shared normals, spectrum
 *                  or rank gate.  This is the default class: pairs first seen
 *                  after the census window are quarantined automatically.
 *
 * Two admission tiers (see @ref runCensus):
 *   - tier 1 (count):  a station supports a pair iff it supplies a usable raw
 *                      code observation in >= ceil(K_eff/2) census epochs (an
 *                      absolute sustained-support floor, which rejects 2-of-2
 *                      coverage from a gappy station); the pair is a candidate
 *                      iff #supporting stations >= effective_min =
 *                      max(2, min(min_stations, n_census_sta)).
 *   - tier 2 (IF-pin): per satellite, the tier-1 candidates must span >= 2
 *                      distinct carrier bands (the distinctness the clock IF pin
 *                      needs); otherwise all of that satellite's candidates are
 *                      demoted to quarantined.  This removes the datum-orphan
 *                      near-null directions at the source instead of at the gate.
 *
 * This struct introduces no stochastic machinery: quarantined states keep the
 * existing birth seed, initial sigma, sat_code_bias_q process noise and warm
 * cache.  @ref runCensus is a pure scan over the synced-epoch vector already in
 * memory; the result is a POD latched once into @ref NetworkRunContext and read
 * as a membership test at the registration site (only @c late_tracked changes
 * after the census).  Off by default (@c network.cb_admission); with the flag
 * off the census never runs (@c active stays false) and the legacy per-epoch
 * path is byte-identical.  Under @c network.streaming the census is skipped
 * with a warning and the legacy gate is used.
 *
 * Sibling of @ref NetworkSbasis / @ref NetworkPhaseDatum.  See
 * docs/SPEC_cb_admission_card.md.
 */

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

struct SyncedEpoch;  ///< rinex/MultiRinexSync.hpp: the census input (forward-declared to keep this header light).

/**
 * @brief Result of the run-latched W6 code-bias admission census: the admitted
 *        (sat,signal) key set plus per-pair verdicts and counters for logging.
 *
 * Default-constructed with @c active==false (the off / census-never-ran state),
 * so a NetworkRunContext holding one by value costs nothing until @ref runCensus
 * fills it.  The admitted set uses the key format of the parameter-set sat-bias
 * states, @c "satellite|signal" (@ref StateKeys::makeSatBiasKey), so the
 * registration site can test membership directly against @c
 * getSatBiasStates() keys.
 */
struct NetworkBiasAdmission {
    /// @brief Per-pair census verdict.
    enum class Verdict {
        Admitted,   ///< Shared border column (datumed + emitted): passed both tiers.
        LowCount,   ///< Quarantined: fewer than @c effective_min supporting stations (tier 1).
        NoIfPin,    ///< Quarantined: satellite's tier-1 candidates span < 2 distinct bands (tier 2).
    };

    /// @brief One (sat,signal) code pair observed during the census window.
    struct Pair {
        std::string key;                     ///< "satellite|signal" (== the parameter-set sat-bias key).
        std::string satellite;               ///< RINEX satellite id (e.g. "C21").
        std::string signal;                  ///< RINEX code obs id (e.g. "C2I").
        int         support{0};              ///< # stations supporting the pair (tier-1 sustained-support count).
        Verdict     verdict{Verdict::LowCount};
    };

    std::set<std::string> admitted;          ///< Datum-grade "sat|sig" keys (fast membership test at the registration site).
    std::set<std::string> seen;              ///< Every "sat|sig" key the census observed (admitted ∪ census-quarantined); answers "was this classified?".
    std::vector<Pair>     census;            ///< Every pair seen in the window (deterministic key order), for logging and telemetry.
    std::set<std::string> late_tracked;      ///< Mutable during the run (not a census output): distinct keys first seen at the registration site after the census; deduplicates n_cb_late_quarantined.
    int  n_admitted{0};                      ///< == admitted.size().
    int  n_quarantined_census{0};            ///< census.size() - n_admitted (distinct pairs quarantined at census close).
    int  census_epochs_used{0};              ///< K_eff actually scanned (clamped to the available span).
    int  census_stations{0};                 ///< Distinct stations appearing anywhere in the census window.
    int  effective_min{0};                   ///< max(2, min(min_stations, census_stations)): the tier-1 station threshold.
    int  sustained_floor{0};                 ///< ceil(K_eff/2): per-station epoch count a station needs to "support" a pair.
    bool active{false};                      ///< Flag armed and the census scanned >= 1 epoch.

    /// @brief True iff @p key ("satellite|signal") was admitted as a shared column.
    bool isAdmitted(const std::string& key) const { return admitted.count(key) != 0; }

    /**
     * @brief Run the one-shot admission census over the census window
     *        @c [i0, min(i0+census_epochs, synced.size())).
     *
     * Pure scan: reads @p synced, allocates nothing outside the returned POD,
     * and is deterministic (std::map/std::set iteration order), so the
     * classification is reproducible from run to run.  The usable-observation
     * predicate over @c SyncedEpoch::by_station -> @c RinexObservation::
     * measurements is: 3-char id, a code observation (@c obsTypeOf is
     * @c ObsType::Code), no '_', finite nonzero value; GLONASS satellites
     * (@c systemOfSat is @c System::GLONASS) are skipped (no OSB scope).
     * Distinctness for tier 2 uses the physical band of each code
     * (@c SignalId::band): two codes are distinct iff their carrier bands differ,
     * the criterion @ref NetworkSbasis::selectIfPinPair applies via carrier
     * frequency.  A same-band co-channel pair such as C1P and C1X is not
     * distinct, and a code that does not parse or whose band is unknown does not
     * count.
     *
     * @param synced        The whole in-memory synced-epoch vector.
     * @param i0            Index of the first epoch the run loop will process
     *                      (after the @c start_offset_hours fast-forward), so the
     *                      census classifies on data the loop actually sees.
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

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the KP prior and the CB-ADM code-pair admission.
 *
 * The admitted-OSB physical-bound prior and the CB-ADM code-pair admission
 * census with its orphan window.
 *
 * Gate: each section under its own condition, as in the teardown:
 * `config_.networkCbKaulaPrior()`; `config_.networkCbAdmission()`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkBiasAdmissionRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- Solve stage 2 and 6 helpers ----

struct NetworkRun;
struct NetworkEpoch;
struct EpochWorkspace;   // network/driver/RunTypes.hpp

/**
 * @brief The physical-bound prior on the admitted OSB columns: folded after the W6 gauge
 *        rows and before the rank gate, through the diagonal prior channel (K3), and captured
 *        for the within-epoch replay.  No-op with network.cb_kaula_prior off.
 * @param run The run: the estimator is folded; ctx counters (n_kp_epochs, n_kp_cols) are written.
 * @param ep  The epoch, read: columns (registered_code_keys).
 * @param ws  [in/out] The replay capture (cap_priors, cap_prior_kinds).
 */
void networkFoldCodeBiasKaulaPrior(NetworkRun& run, const NetworkEpoch& ep, EpochWorkspace& ws);

/**
 * @brief Stage 6 of the network solve: the one-shot K2 anti-fragility margin of the KP prior
 *        against the rank-gate cutoff, logged once.
 * @param run The run: ctx counters (kp_margin_logged) are written; the log.
 */
void networkKaulaMargin(NetworkRun& run);

struct NetworkRun;

/**
 * @brief Runs the code-bias admission census once before the loop and logs its table.
 *
 * Armed by network.cb_admission with the satellite code-bias slice; refused, with a warning,
 * under streaming.  Setup builder: called once from
 * Application::runNetwork, in the order scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.ctx.admission and its two counters.
 */
void networkCbAdmissionCensus(NetworkRun& run);

/// @brief Run-scope counters of BiasAdmission, a base of NetworkServiceCounters (network/state/RunContext.hpp).
struct BiasAdmissionCounters {
    // Epochs on which the admitted-OSB physical-bound prior was folded, and the
    // cumulative column count.  Standing invariant #4: an armed KP run with
    // n_kp_epochs == 0 means the prior never reached a solve (no registered code
    // columns, or the W6 block never ran), which clean-looking log output would not
    // reveal.
    size_t n_kp_epochs = 0, n_kp_cols = 0;
    bool   kp_margin_logged = false;   ///< one-shot latch for the K2 margin check

    // Tallies of the W6 code-bias admission census (CB-ADM; run-end observability).
    // Standing invariant #4: inert wiring is visible only through them, so a BDS-armed
    // on run with n_cb_quarantined_census == 0 is a miswire.  n_cb_late_quarantined
    // counts distinct (sat,signal) keys first seen at the registration site after the
    // census window (implicitly quarantined).
    std::size_t n_cb_admitted = 0, n_cb_quarantined_census = 0, n_cb_late_quarantined = 0;
};

} // namespace GnssProcessor
