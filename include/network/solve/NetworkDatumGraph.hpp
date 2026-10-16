#pragma once
/**
 * @file NetworkDatumGraph.hpp
 * @brief Datum graph census: the structural S-basis dimension, predicted from the
 *        epoch's support topology and reconciled against the spectral observation
 *        (docs/SPEC_datum_graph_census.md).
 *
 * Measure-only, off by default, fingerprint-safe.  This translation unit writes no
 * weight, state, datum row or decision.  It does not move
 * @c NetworkSbasis::Plan::declared_dimension and does not feed the null-space gate,
 * SBC or card PA.  Acting on a prediction is the successor card (spec §8); v1
 * measures how good the prediction is before anything depends on it.
 *
 * ## Why a structural census when the dimension is already measured
 *
 * The realized S-basis dimension is measured spectrally: the null-space gate counts
 * near-zero eigenvalues of the reduced shared normals, and SBC
 * (@c NetworkSbasis::completeSbasisDimension) derives the epoch's extra directions
 * from the same matrix.  That machinery gives the count but not the cause.  The
 * cause can be graph topology: the Galileo C/Q-versus-X tracking split partitions
 * the station set per channel family, and a family whose code support thins loses
 * the b↔I anchor for its (sat, σ) phase-bias columns.
 *
 * NGS LASER Guide v2.7 slides 5-62/5-63 state the theorem: the rank deficiency of a
 * fragmented network is @f$d\cdot k@f$ over @f$k@f$ disjoint components ("29
 * independent networks ⇒ 3×29 = 87"), and *"knowing the type of observations
 * involved could indicate the rank of A, avoiding the need to compute it."*  This
 * file implements that statement.
 *
 * A structural predictor provides three things the spectral path does not:
 *  - early warning by identity (which family, which stations, which satellite)
 *    instead of an eigenvalue count;
 *  - a `predicted-vs-observed` contingency (standing invariant #4) that turns a
 *    gate failure into a diagnosis;
 *  - on the SBC+KP recipe, where the spectral extras are suppressed by design, a
 *    live map of where @c network.cb_kaula_prior is load-bearing (the
 *    `predicted only` cell): the population that KP's own K2-margin warning refers
 *    to but cannot name.
 *
 * ## The two tiers
 *
 * **Tier 1: component multiplicity (LASER 5-63).**  Per constellation, union-find
 * over the station–satellite bipartite graph of admitted observation support.  With
 * @f$k@f$ components, each datum gauge of that constellation is needed @f$k@f$
 * times while the declared plan assumes @f$k = 1@f$, so @f$k > 1@f$ predicts
 * @f$(k-1)\cdot d@f$ undeclared extra deficiencies: the disjoint-network case
 * (regional outage, an admission screen acting en masse).  On a global roster
 * @f$k = 1@f$ almost always; the census makes the exception visible.
 *
 * **Tier 2: channel-family code support.**  Per satellite, the epoch's registered
 * bias keys are partitioned by receiver channel family: the RINEX-3 tracking
 * attribute, the last character of the observation code (X versus C/Q in the
 * Galileo split).  A (sat, family) cell with registered phase-bias columns and no
 * registered code-bias column predicts one phase-bias↔slant-iono trade direction:
 * the per-(sat, σ) anchor pins close the b↔N trade but not b↔I, and with no code
 * column in that family nothing else pins it.
 *
 * ## What the census does not claim
 *
 * Topology sees zero support; it cannot see thin support.  The orphan class (the
 * "CB+CLK, zero-PB-weight" direction at @f$\lambda \approx 10^{-6}@f$) is a weight
 * statement, not a structure statement, and predicting it would need the numbers
 * the spectral path already has.  The census therefore under-claims by
 * construction, and the `observed only` cell measures the cost of that choice.
 *
 * ## Deviation from the spec: a second tier-2 predicate
 *
 * Spec §2 defines tier 2 on registered code keys.  A registered key is a column,
 * and the deficient directions of the Galileo split carried live CB components
 * (`PB:E03:L5X:0.35 CB:E03:C5X:0.35 …`): the columns existed and their row support
 * thinned.  The strict key predicate can therefore be empty on the run §6.3 grades,
 * as the spec anticipates ("a low number falsifies the family-partition definition,
 * not the card").
 *
 * This implementation evaluates both predicates in one pass over the same partition:
 *  - `zero_keys`: the spec's strict predicate.  It alone drives the contingency
 *    cells, the events and the headline coverage number.
 *  - `zero_support`: a weaker companion.  The family's registered code columns
 *    carry zero admitted observables anywhere in the network this epoch.  It has
 *    its own CSV columns, counter and contingency and is never mixed into the
 *    primary cells.
 * The support count includes only observables that have a registered code-bias
 * column, so `zero_keys ⇒ zero_support` holds exactly (no column ⇒ no supported
 * observable).  The two predicates are nested, and the pair answers §6.3's
 * falsification question from a single run.
 *
 * ## Precondition for both predicates: a code-bias layer
 *
 * "No registered code key in this family" means "nothing anchors b↔I" only when the
 * producer estimates satellite code biases.  With @c network.estimate_sat_code_bias
 * off there are no code columns, the code observations pin the slant ionosphere
 * directly, and the trade direction does not exist, yet the literal predicate would
 * fire on every (sat, family) cell.  Both predicates are therefore held off, and the
 * cell is counted, for a constellation that registered no code-bias column this
 * epoch (@ref NetDatumGraphPrediction::cb_layer).
 *
 * ## Cost, and why the support source is `modeled` and not the row stacks
 *
 * The assembled row stacks are the obvious edge source, but @c classifyRow scans
 * every shared column of every row (@c A_s is column-major, so the walk strides
 * badly); the prefit census measures 7.6 ms/epoch at 63 stations doing it, fifteen
 * times this card's 0.5 ms budget, for a graph that needs no residuals, weights or
 * Jacobian.  The census walks @c modeled[station] instead: the per-station modeled
 * observation list, whose `observed` map is keyed by full RINEX-3 code and from
 * which the measurement admission screen has already removed every rejected
 * observable.  That is O(#observations) with the satellite and signal code in hand:
 * about 1300 map entries at 63 stations, against about 7500 rows × 950 columns.
 *
 * The proxy has one gap: an observable present in `modeled` can still fail to
 * produce a row inside @c computeMeasurementUpdate.  Edges therefore require that
 * the station was folded this epoch (@c est_index_of >= 0) and that the observable
 * appears in both the `observed` and `modeled` maps.  The remaining gap is a known
 * limit of the proxy; the census does not measure it.
 */

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "common/PppObservation.hpp"
#include "config/GnssConfig.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "network/solve/NetworkSbasis.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct NetworkRunContext;   // network/state/NetworkRunContext.hpp; included in the .cpp only
struct NetworkRunTraces;    // network/diag/NetworkRunTraces.hpp; included in the .cpp only

// ---------------------------------------------------------------------------
//  Shared constants
// ---------------------------------------------------------------------------

/// Number of components listed individually in a `network_datum_graph.csv` row
/// before the remainder collapses to a `+N more` token (spec §3 INC-2: "≤ 4 listed +
/// count").  The count `k` is never truncated; only the roster is.
inline constexpr int kDatumGraphCompsListed = 4;

/// Rendering of a signal code that carries no RINEX-3 tracking attribute (a
/// RINEX-2 style two-character code).  Such codes form their own family rather
/// than joining an attributed one: merging them would give an unattributed code
/// column credit for supporting, for example, an X-family phase bias.
inline constexpr const char* kDatumGraphNoFamily = "-";

// ---------------------------------------------------------------------------
//  Tier 1: component multiplicity over the admitted support graph
// ---------------------------------------------------------------------------

/// One admitted (station, satellite) support edge of the bipartite graph.
struct NetDatumGraphEdge {
    int         station{-1};   ///< station roster index (the left vertex of the bipartite graph).
    std::string sat;           ///< satellite id, e.g. "E03" (the right vertex).
};

/// One connected component of one constellation's support graph.
struct NetDatumGraphComponent {
    int         n_stations{0};  ///< stations in this component.
    int         n_sats{0};      ///< satellites in this component.
    std::string sample_sat;     ///< lexicographically smallest satellite; the component's name, stable across epochs.
    int         sample_station{-1}; ///< smallest station roster index in the component (−1 = none); the caller resolves the name.
};

/// One constellation's tier-1 census for one epoch.
struct NetDatumGraphConsCensus {
    char cons{'?'};             ///< constellation letter (satellite id's first char).
    int  k{0};                  ///< connected components (the LASER 5-63 multiplicity).
    int  n_stations{0};         ///< distinct stations with ≥1 admitted edge.
    int  n_sats{0};             ///< distinct satellites with ≥1 admitted edge.
    int  n_edges{0};            ///< admitted (station, satellite) edges.
    /// Every component, in descending order of (n_stations + n_sats) with a
    /// smallest-satellite tie-break, so the ordering is deterministic across
    /// epochs and thread counts.  Size == @ref k; the CSV lists the first
    /// @ref kDatumGraphCompsListed.
    std::vector<NetDatumGraphComponent> comps;
};

/**
 * @brief Union-find the admitted support graph, per constellation.
 *
 * On the shared @ref arcgraph::Dsu (card AGT; path compression without union by
 * size, so O(E log V) amortized at worst), which links the station under the
 * satellite; every output depends on the partition alone.  Vertices with no edge are
 * not created: a satellite no station tracks and a station that folded nothing
 * belong to no component, and counting them would inflate @f$k@f$ with vertices
 * that carry no datum.
 *
 * @param edges Admitted support edges, in any order (the result does not depend
 *              on the order, and the component order is made deterministic by
 *              the sort documented on @ref NetDatumGraphConsCensus::comps).
 * @return Per-constellation census, keyed by the satellite id's first
 *         character.  A constellation with no edges is absent, not present with
 *         @f$k = 0@f$.
 */
std::map<char, NetDatumGraphConsCensus>
netDatumGraphComponents(const std::vector<NetDatumGraphEdge>& edges);

// ---------------------------------------------------------------------------
//  Tier 2: channel-family code support
// ---------------------------------------------------------------------------

/**
 * @brief The receiver channel family of a RINEX observation code.
 *
 * The RINEX-3 tracking attribute is the third character ("L5X" → "X",
 * "C1C" → "C").  This partition separates the stations in the Galileo tracking
 * split: receivers tracking the combined I+Q channel register X-family
 * observables, pilot-tracking receivers register C/Q, and the two populations are
 * disjoint station sets.
 *
 * The families are kept as the raw attribute rather than grouped into
 * "pilot versus combined": grouping is a receiver-technology judgement that would
 * have to be made per constellation and per band, and the observed attribution
 * (an X-only direction spanning L5X/L7X/L8X) is already exact at attribute
 * granularity.
 *
 * @param signal Full observation code, e.g. "L5X" / "C1C".  A code shorter than
 *               three characters, or one whose third character is not a letter,
 *               has no attribute and maps to @ref kDatumGraphNoFamily.
 */
std::string netDatumGraphFamily(std::string_view signal);

/// One tier-2 (satellite × channel-family) cell and its two predicates.
struct NetDatumGraphPrediction {
    std::string sat;               ///< satellite id.
    std::string family;            ///< tracking attribute, or @ref kDatumGraphNoFamily.
    int  n_pb{0};                  ///< registered satellite phase-bias columns in this cell.
    int  n_cb{0};                  ///< registered satellite code-bias columns in this cell.
    /// Admitted code observables in this cell, summed over stations, counting
    /// only observables whose (satellite, signal) has a registered code-bias
    /// column.  That restriction makes @ref zero_keys imply @ref zero_support
    /// exactly.
    int  n_code_support{0};
    bool cb_layer{false};          ///< this constellation registered ≥1 satellite code-bias column this epoch, the precondition of both predicates (see the file banner).  False ⇒ both are held off and the cell is counted in cells_no_cb_layer.
    bool zero_keys{false};         ///< spec §2 predicate: @c cb_layer, @c n_pb > 0 and @c n_cb == 0.
    bool zero_support{false};      ///< companion predicate: @c cb_layer, @c n_pb > 0 and @c n_code_support == 0 (implied by @ref zero_keys).
    /// "SAT/FAMILY": the join key, the event identity and the CSV token, spelled
    /// the same way everywhere.
    std::string id() const { return sat + "/" + family; }
};

/**
 * @brief Partition the epoch's registered bias keys by (satellite, family) and
 *        evaluate both tier-2 predicates.
 *
 * @param shared_phase_keys   This epoch's registered satellite phase-bias state
 *                            keys ("<sat>|<signal>|PB").  Unparsable keys are
 *                            skipped, never guessed.
 * @param registered_code_keys This epoch's registered (satellite, signal) code
 *                            columns.
 * @param code_support        Admitted code-observable counts keyed by
 *                            @c "SAT|FAMILY" (see
 *                            @ref netDatumGraphSupportKey), restricted to
 *                            observables with a registered code column.  A cell
 *                            absent from the map has zero support.
 * @return Every (sat, family) cell that carries at least one registered PB
 *         column, in ascending order of @ref NetDatumGraphPrediction::id.  Cells
 *         with no phase bias cannot host a b↔I trade and are not cells of this
 *         census.
 */
std::vector<NetDatumGraphPrediction> netDatumGraphChannelFamilies(
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::map<std::string, int>& code_support);

/// Key convention of the @c code_support map: @c "SAT|FAMILY".
inline std::string netDatumGraphSupportKey(std::string_view sat,
                                           std::string_view family) {
    std::string k(sat);
    k.push_back('|');
    k.append(family);
    return k;
}

// ---------------------------------------------------------------------------
//  Spectral side: what the epoch observed
// ---------------------------------------------------------------------------

/// One observed near-null direction beyond the declared dimension, as reported
/// by SBC (@c NetworkSbasis::GaugeCompletion::Dir), reduced to what the join
/// needs.
struct NetDatumGraphDir {
    double      ev{0.0};       ///< the direction's eigenvalue in the reduced shared normals.
    bool        skipped{false};///< SBC skipped it (the unseparated machine-zero orphan class, left to the drop-and-heal).
    std::string top_label;     ///< label of the heaviest participating column; names the direction when it has no (sat, family) identity.
    std::string detail;        ///< SBC's "label:span,…" attribution, verbatim (the same string its `sbc_engage` event carries).
    /// "SAT/FAMILY" of every named participant, ascending and unique.  Empty for
    /// a direction whose participants are all SH coefficients or unattributed
    /// columns; such a direction can only land in the `observed only` cell.
    std::vector<std::string> ids;
};

/**
 * @brief The epoch's spectral observation, handed forward from the solve glue.
 *
 * Written only when the census is armed (@c ctx.knobs.dg_on) and cleared at the
 * top of every epoch, so a skipped or withheld epoch never hands a stale
 * observation to the next one.  Bounded: SBC caps its own direction count at
 * @c network.sbasis_complete_max and its per-direction attribution at six
 * participants, so this struct inherits both bounds and does not grow with the
 * network.
 */
struct NetDatumGraphSpectral {
    bool valid{false};      ///< a rank evaluation happened this epoch (false ⇒ the epoch never reached the gate).
    bool from_sbc{false};   ///< the directions carry identities (SBC armed).  False ⇒ @ref dirs is empty and only the counts can be reconciled.
    int  observed{-1};      ///< realized near-null count before completion (SBC's own count when SBC is armed, else the gate's first evaluation).
    int  declared{0};       ///< @c NetworkSbasis::Plan::declared_dimension for the epoch.
    std::vector<NetDatumGraphDir> dirs;   ///< observed extra directions (SBC's constrained ones and its skipped orphans alike).
    void clear() {
        valid = false;
        from_sbc = false;
        observed = -1;
        declared = 0;
        dirs.clear();
    }
    /// Extra directions the spectrum carried beyond the declared dimension, by
    /// count.  Never negative: @c observed < @c declared is the gate's
    /// "over-specified" verdict, a different finding, and is not an extra.
    int extras() const {
        return (observed >= 0 && observed > declared) ? observed - declared : 0;
    }
};

/**
 * @brief Parse SBC's `detail` attribution string into (sat, family) identities.
 *
 * The string is @c "LABEL:span,LABEL:span,…" over the direction's heaviest
 * participants, with labels in @c NetworkEstimator::sharedColumnLabel form:
 * @c "PB:<sat>:<signal>", @c "CB:<sat>:<signal>", @c "CLK:<sat>",
 * @c "SH[2]:<trig>:<n>:<m>".  Only the two bias kinds carry a signal and
 * therefore a family; a clock or SH participant contributes no identity.
 *
 * Parsing the string the `sbc_engage` event carries, rather than widening
 * @c GaugeCompletion::Dir, is deliberate: the census and the event then agree on
 * a direction's identity by construction, and the SBC path costs nothing extra
 * when the census is off.  The inherited limit is SBC's six-participant cap: a
 * widely spread direction is identified by its heaviest six participants.
 *
 * @return Unique "SAT/FAMILY" identities, ascending.
 */
std::vector<std::string> netDatumGraphDetailIds(std::string_view detail);

// ---------------------------------------------------------------------------
//  Reconciliation, the card's deliverable (INC-1)
// ---------------------------------------------------------------------------

/**
 * @brief The three contingency cells of one epoch.
 *
 * The two partitions are over different populations and are reported
 * separately, because the join is many-to-many (one direction can span several
 * (sat, family) cells, and one cell can appear in several directions):
 *
 *  - over predictions: @ref pred_observed + @ref pred_only = every prediction;
 *  - over directions:  @ref obs_only + @ref n_obs_matched = every direction.
 *
 * Collapsing them into a single 2×2 table would double-count one side, the
 * accounting error spec §5.4 describes.
 */
struct NetDatumGraphContingency {
    std::vector<std::string> pred_observed; ///< predicted identities named by ≥1 observed direction; the census explains these.
    std::vector<std::string> pred_only;     ///< predicted identities no direction named.  With KP armed these form the KP-load-bearing map; without it, they are over-prediction (a finding about the predictor).
    std::vector<std::string> obs_only;      ///< observed directions no prediction named, by identity (or by @c top_label when the direction has none): the weak-support, spectral-only boundary.
    int n_obs_matched{0};                   ///< observed directions naming ≥1 prediction.
};

/// Event name for a `predicted only` identity.  With KP armed the direction
/// exists structurally and @c network.cb_kaula_prior holds it above the rank
/// cutoff, so the cell is the KP-load-bearing map spec §3 asks for; without KP
/// nothing lifts it and the same cell is a finding about the predictor.  One
/// function supplies the label so that it always matches the arming it describes.
inline const char* netDatumGraphPredOnlyEvent(bool kp_armed) {
    return kp_armed ? "dg_kp_load_bearing" : "dg_predicted_only";
}

/// Action field paired with @ref netDatumGraphPredOnlyEvent, under the same contract.
inline const char* netDatumGraphPredOnlyAction(bool kp_armed) {
    return kp_armed ? "kp_lifted" : "over_predict";
}

/**
 * @brief Join predictions to observed directions.
 *
 * @param predicted_ids "SAT/FAMILY" identities predicted this epoch (any order;
 *                      duplicates are collapsed).
 * @param dirs          The epoch's observed extra directions.
 */
NetDatumGraphContingency netDatumGraphReconcile(
    const std::vector<std::string>& predicted_ids,
    const std::vector<NetDatumGraphDir>& dirs);

// ---------------------------------------------------------------------------
//  Run-end telemetry (standing invariant #4)
// ---------------------------------------------------------------------------

/**
 * @brief Cumulative census counters.
 *
 * Invariant #4 applied to a predictor: the expected tier-1 reading on a fault-free
 * global roster is @f$k = 1@f$ everywhere with zero predictions, which cannot be
 * told apart from a seam that never ran unless the run reports it.  Both
 * @ref epochs and @ref epochs_quiet are therefore reported, and the teardown line
 * prints how many epochs the census ran and how many of them were quiet.
 */
struct NetworkDatumGraphTallies {
    long long epochs = 0;            ///< census passes (the denominator).
    long long epochs_quiet = 0;      ///< passes with k ≤ 1 on every constellation and no prediction on either tier-2 predicate (ran, saw nothing).
    long long epochs_frag = 0;       ///< passes with k > 1 on at least one constellation.
    int       max_k = 0;             ///< largest component count seen on any constellation.
    long long pred_tier1 = 0;        ///< cumulative (k−1)·d predicted deficiencies.
    long long pred_tier2 = 0;        ///< cumulative strict (zero registered code key) predictions.
    long long pred_support = 0;      ///< cumulative companion (zero admitted code support) predictions; a superset of @ref pred_tier2.
    long long cells_tier2 = 0;       ///< (sat, family) cells examined (the tier-2 denominator).
    long long cells_no_cb_layer = 0; ///< cells whose constellation registered no code-bias column: both predicates held off and the cell counted here rather than omitted.
    long long obs_dirs = 0;          ///< observed extra directions with identities (the join's right-hand population).
    long long obs_extras_count = 0;  ///< observed extras by count (observed − declared), summed; available even when SBC is off.
    long long epochs_count_only = 0; ///< passes whose spectral side had extras but no identities (SBC disarmed, or armed and refused before attributing): count-level reconciliation only.
    long long epochs_no_spectral = 0;///< passes with no rank evaluation at all (the epoch never reached the gate).
    long long cell_pred_obs = 0;     ///< contingency: predicted ∧ observed.
    long long cell_pred_only = 0;    ///< contingency: predicted only.
    long long cell_obs_only = 0;     ///< contingency: observed only.
    long long cell_obs_matched = 0;  ///< directions that named ≥1 prediction.
    long long support_pred_obs = 0;  ///< companion-predicate contingency: predicted ∧ observed.
    long long support_pred_only = 0; ///< companion-predicate contingency: predicted only.
    long long events = 0;            ///< DATUMGRAPH events emitted.
    long long events_held = 0;       ///< events suppressed by the per-identity holdoff.
    long long edges = 0;             ///< admitted support edges walked (cost and wiring evidence).
    double    ms = 0.0;              ///< cumulative census cost (ms).
    double    ms_max = 0.0;          ///< worst single pass (ms), checked against the §7 budget.
};

// ---------------------------------------------------------------------------
//  The seam
// ---------------------------------------------------------------------------

struct NetworkRun;              // network/driver/NetworkRun.hpp (card NOM3 step EP1)
struct NetworkEpoch;            // network/driver/NetworkEpoch.hpp (card NOM3 step EP1)

/**
 * @brief One epoch's datum-graph census: predict, reconcile, emit.
 *
 * Call from the driver's SB_FRAMEDIAG block after the solve: registration is
 * final by then, the admission screen has finished removing observables, and
 * @c ctx.carry.dg_spec holds this epoch's spectral observation.  The driver
 * gates the call on @c ctx.knobs.dg_on, so an unarmed run never enters this
 * translation unit.
 *
 * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
 * replace the explicit parameters; the definition binds each former parameter in its preamble,
 * under its former name.
 *
 * @param run The run (network/driver/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
 *            run.traces (traces). Read: run.config (config_), run.log (log),
 *            run.setup.stations (stations).
 * @param ep The epoch (network/driver/NetworkEpoch.hpp), read only. Bindings: ep.se (se),
 *           ep.pass_a.modeled (modeled), ep.assembly.est_index_of (est_index_of),
 *           ep.solve.plan (plan), ep.pass_a.shared_phase_keys (shared_phase_keys),
 *           ep.columns.registered_code_keys (registered_code_keys),
 *           ep.columns.registered_clock_sats (registered_clock_sats), ep.solve.datum_changed
 *           (datum_changed).
 *
 * The former parameters, by binding:
 *   - @p ctx = run.ctx: Run context: knobs, the spectral hand-off, the event-holdoff clock,
 *     the per-era identity memory and every counter live here.
 *   - @p config_ = run.config: Full configuration. Reads @c network.cb_kaula_prior (for the
 *     KP-load-bearing label) and @c network.clock_datum_broadcast_anchored (the tier-1 datum
 *     mode); the @c network.datum_graph_* knobs arrive through @c ctx.knobs.
 *   - @p log = run.log: Main run logger (one-shot arming line).
 *   - @p traces = run.traces: Run traces (`network_datum_graph.csv` + `network_events.csv`).
 *   - @p se = ep.se: This synced epoch (week/tow stamp).
 *   - @p stations = run.setup.stations: Station roster; names only, for component identities.
 *   - @p modeled = ep.pass_a.modeled: Per-station modeled observations: the admitted support
 *     source (see the file banner).
 *   - @p est_index_of = ep.assembly.est_index_of: Station → kernel slot map (−1 = not folded
 *     this epoch ⇒ contributes no edge).
 *   - @p plan = ep.solve.plan: This epoch's datum plan: supplies the declared dimension and,
 *     through @c plan.clock_pivot, the per-constellation gauge count d in pin mode.
 *   - @p shared_phase_keys = ep.pass_a.shared_phase_keys: Registered satellite phase-bias
 *     state keys.
 *   - @p registered_code_keys = ep.columns.registered_code_keys: Registered (satellite,
 *     signal) code columns.
 *   - @p registered_clock_sats = ep.columns.registered_clock_sats: Registered satellite clock
 *     columns. They set the tier-1 gauge count d under the sum-to-zero clock datum, where
 *     @c plan.clock_pivot is empty; under the broadcast-anchored datum d = 0 and they are
 *     unused.
 *   - @p datum_changed = ep.solve.datum_changed: This epoch re-referenced the datum: the era
 *     boundary at which the `predicted only` identity memory resets (the boundary at which SBC
 *     also re-seeds its continuity targets).
 */
void networkDatumGraphCensus(NetworkRun& run, const NetworkEpoch& ep);

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

struct NetworkRunEndView;

/**
 * @brief Run-end report: the datum graph census.
 *
 * The datum graph census: passes, quiet and fragmented epochs, predicted against
 * observed deficiencies, its cost, and the miswire and coverage warnings.
 *
 * Gate: returns 0 at once unless `ctx.knobs.dg_on`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
int networkDatumGraphRunEnd(const NetworkRunEndView& v);

// ---- End of the run-end reports ----

// ---- Solve stage 7 helper (card NOM3 step SG) ----

/**
 * @brief The datum graph census's spectral hand-off from S-basis completion: the realized
 *        near-null count before completion, with each direction's identities (constrained and
 *        skipped).  Measure-only; armed-only (network.trace_datum_graph).
 * @param ctx  The run context: carry (dg_spec) is written.
 * @param sbc  The completion's result (read).
 * @param plan The epoch's datum plan (read: declared_dimension).
 */
void networkDatumGraphSpectralHandoff(NetworkRunContext& ctx, const NetworkSbasis::GaugeCompletion& sbc,
                                      const NetworkSbasis::Plan& plan);

} // namespace GnssProcessor
