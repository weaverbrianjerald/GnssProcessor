#pragma once
/**
 * @file NetworkDatumGraph.hpp
 * @brief The DATUM GRAPH CENSUS — structural S-basis dimension, PREDICTED from
 *        the epoch's support topology and reconciled against the SPECTRAL
 *        observation (docs/SPEC_datum_graph_census.md).
 *
 * Measure-only, default-OFF, fingerprint-SAFE.  Nothing in this translation
 * unit writes a weight, a state, a datum row or a decision.  In particular it
 * does NOT move @c NetworkSbasis::Plan::declared_dimension, does not feed the
 * null-space gate, does not feed SBC, and does not feed Card PA — acting on a
 * prediction is the successor card (spec §8), and the whole point of v1 is to
 * measure how good the prediction is BEFORE anything depends on it.
 *
 * ## Why a structural census when the dimension is already measured
 *
 * The realized S-basis dimension is measured SPECTRALLY today: the null-space
 * gate counts near-zero eigenvalues of the reduced shared normals, and SBC
 * (@c NetworkSbasis::completeSbasisDimension) derives the epoch's extra
 * directions from the same matrix.  That machinery answers *how many*.  Nothing
 * answers *why*, and on 2026-08-02 the why turned out to be graph topology: the
 * Galileo C/Q-vs-X tracking split partitions the STATION set per channel
 * family, and a family whose code support thins loses the b↔I anchor for its
 * (sat, σ) phase-bias columns.
 *
 * NGS LASER Guide v2.7 slides 5-62/5-63 state the theorem form — the rank
 * deficiency of a fragmented network is @f$d\cdot k@f$ over @f$k@f$ disjoint
 * components ("29 independent networks ⇒ 3×29 = 87"), and, the operative
 * sentence, *"knowing the type of observations involved could indicate the rank
 * of A, avoiding the need to compute it."*  This file is that sentence, wired.
 *
 * A structural predictor buys three things the spectral path cannot:
 *  - early warning in PLAIN IDENTITIES (which family, which stations, which
 *    satellite) instead of an eigenvalue count;
 *  - a standing-invariant-#4 `predicted-vs-observed` contingency that turns
 *    "the gate threw" into a diagnosis;
 *  - on the SBC+KP recipe, where the spectral extras are suppressed by design,
 *    a live map of WHERE @c network.cb_kaula_prior is load-bearing (the
 *    `predicted only` cell) — exactly the population KP's own K2-margin WARN
 *    worries about without being able to name.
 *
 * ## The two tiers
 *
 * **Tier 1 — component multiplicity (LASER 5-63 verbatim).**  Per
 * constellation, union-find over the station–satellite bipartite graph of
 * ADMITTED observation support.  @f$k@f$ components ⇒ each datum gauge of that
 * constellation is needed @f$k@f$ times, while the declared plan assumes
 * @f$k = 1@f$, so @f$k > 1@f$ predicts @f$(k-1)\cdot d@f$ silent extra
 * deficiencies — the classic disjoint-network case (regional outage, an
 * admission screen acting en masse).  On a global roster this reads @f$k = 1@f$
 * essentially always; the census exists so that the day it does not is not
 * silent.
 *
 * **Tier 2 — channel-family code support (the Aug-2 mechanism).**  Per
 * satellite, partition the epoch's registered bias keys by receiver CHANNEL
 * FAMILY — the RINEX-3 tracking attribute, the last character of the
 * observation code, which is what split the stations on Aug-2 (X vs C/Q).  A
 * (sat, family) cell that has registered PHASE-bias columns but NO registered
 * code-bias column predicts one phase-bias↔slant-iono trade direction: the
 * per-(sat, σ) anchor pins close b↔N but not b↔I, and with no code column in
 * that family nothing else does either.
 *
 * ## What the census deliberately does NOT claim
 *
 * Topology sees ZERO support.  It cannot see THIN support, and the Aug-2 record
 * is explicit that the orphan class (the "CB+CLK, zero-PB-weight" direction at
 * @f$\lambda \approx 10^{-6}@f$) is a WEIGHT statement, not a structure
 * statement.  Predicting it would need the numbers the spectral path already
 * has.  So the census under-claims by construction and the `observed only` cell
 * MEASURES the cost of that choice — that asymmetry is part of the measurement,
 * not a defect.
 *
 * ## Deviation from the spec, recorded (the SECOND tier-2 predicate)
 *
 * Spec §2 defines tier 2 on *registered code KEYS*.  A registered key is a
 * COLUMN, and on the Aug-2 lineage the deficient directions carried live CB
 * components (`PB:E03:L5X:0.35 CB:E03:C5X:0.35 …`), i.e. the columns existed
 * and it was their ROW SUPPORT that thinned.  The strict key predicate can
 * therefore be empty on exactly the run §6.3 grades, and the spec anticipates
 * that outcome ("a low number falsifies the family-partition definition, not
 * the card").
 *
 * Rather than run the gate blind, this implementation measures BOTH, in one
 * pass, from the same partition:
 *  - `zero_keys` — the spec's STRICT predicate.  It alone drives the
 *    contingency cells, the events and the headline coverage number.
 *  - `zero_support` — a strictly WEAKER companion: the family's registered code
 *    columns carry ZERO admitted observables anywhere in the network this
 *    epoch.  Reported in its own CSV columns and its own counter, joined in its
 *    own contingency, and never mixed into the primary cells.
 * The support count deliberately counts only observables that HAVE a registered
 * code-bias column, which makes `zero_keys ⇒ zero_support` hold exactly (no
 * column ⇒ no supported observable).  The two predicates are therefore nested,
 * not competing, and the pair answers §6.3's falsification question with data
 * from the same run instead of a second field cycle.
 *
 * ## The precondition both predicates need: a code-bias LAYER
 *
 * "No registered code key in this family" only means "nothing anchors b↔I" when
 * the producer estimates satellite code biases at all.  With
 * @c network.estimate_sat_code_bias off there are no code columns anywhere, the
 * code OBSERVATIONS pin the slant ionosphere directly, and the trade direction
 * the predicate names does not exist — yet the literal predicate would fire on
 * every single (sat, family) cell in the run.  Both predicates are therefore
 * held OFF, and COUNTED, for a constellation that registered no code-bias
 * column this epoch (@ref NetDatumGraphPrediction::cb_layer).
 *
 * ## Cost, and why the support source is `modeled` and not the row stacks
 *
 * The obvious edge source is the assembled row stacks, but @c classifyRow scans
 * every shared column of every row (@c A_s is column-major, so that walk strides
 * badly) and the prefit census measures 7.6 ms/epoch at 63 stations doing it —
 * fifteen times this card's 0.5 ms budget for a graph that needs no residuals,
 * no weights and no Jacobian.  The census therefore walks
 * @c modeled[station] — the per-station modeled observation list, whose
 * `observed` map is keyed by full RINEX-3 code and from which the measurement
 * admission screen has already REMOVED every rejected observable.  That is
 * O(#observations) with the satellite and the signal code in hand: ~1300 map
 * entries at 63 stations against ~7500 rows × ~950 columns.
 *
 * The proxy is honest about its one gap: an observable present in `modeled` can
 * still fail to produce a row inside @c computeMeasurementUpdate.  Edges are
 * therefore gated on the station having been FOLDED this epoch
 * (@c est_index_of >= 0) and on the observable appearing in BOTH the `observed`
 * and `modeled` maps, and the residual gap is named in the run-end line rather
 * than papered over.
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
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "rinex/MultiRinexSync.hpp"

namespace GnssProcessor {

struct NetworkRunContext;   // app/NetworkRunContext.hpp — included in the .cpp only
struct NetworkRunTraces;    // logging/NetworkRunTraces.hpp — included in the .cpp only

// ---------------------------------------------------------------------------
//  Shared constants
// ---------------------------------------------------------------------------

/// Components listed individually in a `network_datum_graph.csv` row before the
/// remainder collapses to a `+N more` token (spec §3 INC-2: "≤ 4 listed +
/// count").  The COUNT `k` is never truncated — only the roster is.
inline constexpr int kDatumGraphCompsListed = 4;

/// Rendering of a signal code that carries no RINEX-3 tracking attribute (a
/// RINEX-2 style two-character code).  Such codes form their OWN family rather
/// than joining an attributed one: merging them would silently give an
/// unattributed code column credit for supporting an X-family phase bias.
inline constexpr const char* kDatumGraphNoFamily = "-";

// ---------------------------------------------------------------------------
//  TIER 1 — component multiplicity over the admitted support graph
// ---------------------------------------------------------------------------

/// One admitted (station, satellite) support edge of the bipartite graph.
struct NetDatumGraphEdge {
    int         station{-1};   ///< station roster index (the bipartite LEFT vertex).
    std::string sat;           ///< satellite id, e.g. "E03" (the RIGHT vertex).
};

/// One connected component of one constellation's support graph.
struct NetDatumGraphComponent {
    int         n_stations{0};  ///< stations in this component.
    int         n_sats{0};      ///< satellites in this component.
    std::string sample_sat;     ///< lexicographically smallest satellite — the component's stable NAME across epochs.
    int         sample_station{-1}; ///< smallest station ROSTER INDEX in the component (−1 = none); the caller resolves the name.
};

/// One constellation's tier-1 census for one epoch.
struct NetDatumGraphConsCensus {
    char cons{'?'};             ///< constellation letter (satellite id's first char).
    int  k{0};                  ///< connected components — the LASER 5-63 multiplicity.
    int  n_stations{0};         ///< distinct stations with ≥1 admitted edge.
    int  n_sats{0};             ///< distinct satellites with ≥1 admitted edge.
    int  n_edges{0};            ///< admitted (station, satellite) edges.
    /// Every component, DESCENDING by (n_stations + n_sats) with a
    /// smallest-satellite tie-break so the ordering is deterministic across
    /// epochs and thread counts.  Size == @ref k; the CSV lists the first
    /// @ref kDatumGraphCompsListed.
    std::vector<NetDatumGraphComponent> comps;
};

/**
 * @brief Union-find the admitted support graph, per constellation.
 *
 * O(E·α(V)) with a path-compressing, union-by-size forest.  Vertices with no
 * edge do not exist — a satellite nobody tracks and a station that folded
 * nothing are not components of anything, and counting them would inflate
 * @f$k@f$ with vertices that carry no datum.
 *
 * @param edges Admitted support edges, in any order (the result is
 *              order-independent by construction, and the component ORDER is
 *              made deterministic by the sort documented on
 *              @ref NetDatumGraphConsCensus::comps).
 * @return Per-constellation census, keyed by the satellite id's first
 *         character.  A constellation with no edges is absent, not present with
 *         @f$k = 0@f$.
 */
std::map<char, NetDatumGraphConsCensus>
netDatumGraphComponents(const std::vector<NetDatumGraphEdge>& edges);

// ---------------------------------------------------------------------------
//  TIER 2 — channel-family code support
// ---------------------------------------------------------------------------

/**
 * @brief The receiver CHANNEL FAMILY of a RINEX observation code.
 *
 * The RINEX-3 tracking attribute is the third character ("L5X" → "X",
 * "C1C" → "C").  This is the partition that split the station set on Aug-2:
 * Galileo receivers tracking the combined I+Q channel register X-family
 * observables while pilot-tracking receivers register C/Q, and the two
 * populations are disjoint sets of STATIONS.
 *
 * The families are kept as the RAW attribute rather than grouped into
 * "pilot vs combined": grouping is a receiver-technology judgement that would
 * have to be re-made per constellation and per band, and the Aug-2 attribution
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
    int  n_pb{0};                  ///< registered satellite PHASE-bias columns in this cell.
    int  n_cb{0};                  ///< registered satellite CODE-bias columns in this cell.
    /// Admitted code observables in this cell, summed over stations, counting
    /// ONLY observables whose (satellite, signal) has a registered code-bias
    /// column.  That restriction is what makes @ref zero_keys imply
    /// @ref zero_support exactly.
    int  n_code_support{0};
    bool cb_layer{false};          ///< this constellation registered ≥1 satellite code-bias column this epoch — the precondition BOTH predicates need (see the header banner).  False ⇒ both are held off and the cell is counted as suppressed.
    bool zero_keys{false};         ///< SPEC §2 predicate: @c cb_layer, @c n_pb > 0 and @c n_cb == 0.
    bool zero_support{false};      ///< COMPANION predicate: @c cb_layer, @c n_pb > 0 and @c n_code_support == 0 (implied by @ref zero_keys).
    /// "SAT/FAMILY" — the join key, the event identity and the CSV token.  One
    /// spelling everywhere so a reader never has to align two conventions.
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
 *                            observables with a REGISTERED code column.  A cell
 *                            absent from the map has zero support.
 * @return Every (sat, family) cell that carries at least one registered PB
 *         column, ASCENDING by @ref NetDatumGraphPrediction::id — cells with no
 *         phase bias cannot host a b↔I trade and are not cells of this census.
 */
std::vector<NetDatumGraphPrediction> netDatumGraphChannelFamilies(
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::map<std::string, int>& code_support);

/// Key convention of the @c code_support map — @c "SAT|FAMILY".
inline std::string netDatumGraphSupportKey(std::string_view sat,
                                           std::string_view family) {
    std::string k(sat);
    k.push_back('|');
    k.append(family);
    return k;
}

// ---------------------------------------------------------------------------
//  The SPECTRAL side — what the epoch actually observed
// ---------------------------------------------------------------------------

/// One observed near-null direction beyond the declared dimension, as reported
/// by SBC (@c NetworkSbasis::GaugeCompletion::Dir), reduced to what the join
/// needs.
struct NetDatumGraphDir {
    double      ev{0.0};       ///< the direction's eigenvalue in the reduced shared normals.
    bool        skipped{false};///< SBC left it to the drop-and-heal (the unseparated machine-zero orphan class).
    std::string top_label;     ///< heaviest participating column's label — the direction's name when it has no (sat, family) identity at all.
    std::string detail;        ///< SBC's "label:span,…" attribution, verbatim (the same string its `sbc_engage` event carries).
    /// "SAT/FAMILY" of every NAMED participant, ascending and unique.  Empty for
    /// a direction whose participants are all SH coefficients or unattributed
    /// columns — such a direction can only ever land in the `observed only` cell,
    /// which is the honest answer.
    std::vector<std::string> ids;
};

/**
 * @brief The epoch's spectral observation, handed forward from the solve glue.
 *
 * Written ONLY when the census is armed (@c ctx.knobs.dg_on) and cleared at the
 * top of every epoch, so a skipped or withheld epoch can never hand a stale
 * observation to the next one.  Bounded: SBC caps its own direction count at
 * @c network.sbasis_complete_max and its per-direction attribution at six
 * participants, so this struct inherits both bounds and cannot grow with the
 * network.
 */
struct NetDatumGraphSpectral {
    bool valid{false};      ///< a rank evaluation happened this epoch (false ⇒ the epoch never reached the gate).
    bool from_sbc{false};   ///< the directions carry IDENTITIES (SBC armed).  False ⇒ @ref dirs is empty and only the COUNTS can be reconciled.
    int  observed{-1};      ///< realized near-null count PRE-completion (SBC's own count when armed, else the gate's first evaluation).
    int  declared{0};       ///< @c NetworkSbasis::Plan::declared_dimension for the epoch.
    std::vector<NetDatumGraphDir> dirs;   ///< observed EXTRA directions (SBC's constrained ones and its skipped orphans alike).
    void clear() {
        valid = false;
        from_sbc = false;
        observed = -1;
        declared = 0;
        dirs.clear();
    }
    /// Extra directions the spectrum carried beyond the declared dimension, by
    /// COUNT.  Never negative: @c observed < @c declared is the gate's
    /// "over-specified" verdict, a different finding, and is not an extra.
    int extras() const {
        return (observed >= 0 && observed > declared) ? observed - declared : 0;
    }
};

/**
 * @brief Parse SBC's `detail` attribution string into (sat, family) identities.
 *
 * The string is @c "LABEL:span,LABEL:span,…" over the direction's heaviest
 * participants, with labels in @c NetworkEstimator::sharedColumnLabel form —
 * @c "PB:<sat>:<signal>", @c "CB:<sat>:<signal>", @c "CLK:<sat>",
 * @c "SH[2]:<trig>:<n>:<m>".  Only the two BIAS kinds carry a signal and
 * therefore a family; a clock or SH participant contributes no identity.
 *
 * Parsing the same string the `sbc_engage` event carries — rather than widening
 * @c GaugeCompletion::Dir — is deliberate: the census and the event then agree
 * about a direction's identity BY CONSTRUCTION, and the SBC path costs nothing
 * extra when the census is off.  The one inherited limit is SBC's six-participant
 * cap, so a widely spread direction is identified by its heaviest six.  That is
 * named in the run-end line.
 *
 * @return Unique "SAT/FAMILY" identities, ascending.
 */
std::vector<std::string> netDatumGraphDetailIds(std::string_view detail);

// ---------------------------------------------------------------------------
//  INC-1 — the reconciliation (the card's deliverable)
// ---------------------------------------------------------------------------

/**
 * @brief The three contingency cells of one epoch.
 *
 * The two partitions are over DIFFERENT populations and are reported
 * separately on purpose, because the join is many-to-many (one direction can
 * span several (sat, family) cells, and one cell can appear in several
 * directions):
 *
 *  - over PREDICTIONS: @ref pred_observed + @ref pred_only = every prediction;
 *  - over DIRECTIONS:  @ref obs_only + @ref n_obs_matched = every direction.
 *
 * Collapsing them into a single 2×2 would require picking one side to
 * double-count, which is exactly the accounting error spec §5.4 pins.
 */
struct NetDatumGraphContingency {
    std::vector<std::string> pred_observed; ///< predicted identities named by ≥1 observed direction — the census EXPLAINS these.
    std::vector<std::string> pred_only;     ///< predicted identities no direction named.  With KP armed these are the KP-LOAD-BEARING map; without it, over-prediction (a finding about the predictor).
    std::vector<std::string> obs_only;      ///< observed directions no prediction named, by identity (or by @c top_label when the direction has none) — the weak-support / spectral-only boundary.
    int n_obs_matched{0};                   ///< observed directions naming ≥1 prediction.
};

/// Event name for a `predicted only` identity.  With KP armed the direction
/// exists STRUCTURALLY and @c network.cb_kaula_prior is what holds it above the
/// rank cutoff, so the cell is the KP-LOAD-BEARING map spec §3 asks for; without
/// KP there is no lift to attribute it to and the same cell is a finding about
/// the PREDICTOR.  One function, so the label can never disagree with the arming
/// it claims to describe.
inline const char* netDatumGraphPredOnlyEvent(bool kp_armed) {
    return kp_armed ? "dg_kp_load_bearing" : "dg_predicted_only";
}

/// Action field paired with @ref netDatumGraphPredOnlyEvent — same contract.
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
 * Invariant #4 in its sharpest form for a predictor: the EXPECTED reading of
 * tier 1 on a healthy global roster is @f$k = 1@f$ everywhere and zero
 * predictions, which is indistinguishable from a seam that never ran unless the
 * run says so.  Hence @ref epochs and @ref epochs_quiet are BOTH reported, and
 * the teardown line prints "ran N epochs, N of them quiet" rather than nothing.
 */
struct NetworkDatumGraphTallies {
    long long epochs = 0;            ///< census passes (the denominator).
    long long epochs_quiet = 0;      ///< passes with k == 1 on every constellation AND zero tier-2 predictions — the "ran, saw nothing" outcome.
    long long epochs_frag = 0;       ///< passes with k > 1 on at least one constellation.
    int       max_k = 0;             ///< largest component count seen on any constellation.
    long long pred_tier1 = 0;        ///< cumulative (k−1)·d predicted deficiencies.
    long long pred_tier2 = 0;        ///< cumulative STRICT (zero registered code key) predictions.
    long long pred_support = 0;      ///< cumulative COMPANION (zero admitted code support) predictions — a superset of @ref pred_tier2.
    long long cells_tier2 = 0;       ///< (sat, family) cells examined — the tier-2 denominator.
    long long cells_no_cb_layer = 0; ///< cells whose constellation registered no code-bias column: both predicates held OFF and counted, never silently absent.
    long long obs_dirs = 0;          ///< observed extra directions with identities (the join's right-hand population).
    long long obs_extras_count = 0;  ///< observed extras by COUNT (observed − declared), summed — live even when SBC is off.
    long long epochs_count_only = 0; ///< passes whose spectral side had extras but NO identities (SBC disarmed) — count-level reconciliation only.
    long long epochs_no_spectral = 0;///< passes with no rank evaluation at all (the epoch never reached the gate).
    long long cell_pred_obs = 0;     ///< contingency: predicted ∧ observed.
    long long cell_pred_only = 0;    ///< contingency: predicted only.
    long long cell_obs_only = 0;     ///< contingency: observed only.
    long long cell_obs_matched = 0;  ///< directions that named ≥1 prediction.
    long long support_pred_obs = 0;  ///< companion-predicate contingency: predicted ∧ observed.
    long long support_pred_only = 0; ///< companion-predicate contingency: predicted only.
    long long events = 0;            ///< DATUMGRAPH events emitted.
    long long events_held = 0;       ///< …suppressed by the per-identity holdoff.
    long long edges = 0;             ///< admitted support edges walked (cost + wiring evidence).
    double    ms = 0.0;              ///< cumulative census cost (ms).
    double    ms_max = 0.0;          ///< worst single pass (ms) — the §7 budget check.
};

// ---------------------------------------------------------------------------
//  The seam
// ---------------------------------------------------------------------------

/**
 * @brief One epoch's datum-graph census: predict, reconcile, emit.
 *
 * Call from the driver's SB_FRAMEDIAG block AFTER the solve — registration is
 * final by then, the admission screen has finished removing observables, and
 * @c ctx.carry.dg_spec holds this epoch's spectral observation.  The driver
 * gates the call on @c ctx.knobs.dg_on, so an unarmed run never enters this
 * translation unit.
 *
 * @param ctx                  Run context: knobs, the spectral hand-off, the
 *                             event-holdoff clock, the per-era identity memory
 *                             and every counter live here.
 * @param config_              Full configuration (reads @c network.datum_graph_*
 *                             plus @c network.cb_kaula_prior for the
 *                             KP-load-bearing label).
 * @param log                  Main run logger (one-shot arming / degradation lines).
 * @param traces               Run traces (`network_datum_graph.csv` +
 *                             `network_events.csv`).
 * @param se                   This synced epoch (week/tow stamp).
 * @param stations             Station roster — names only, for component identities.
 * @param modeled              Per-station modeled observations: the ADMITTED
 *                             support source (see the header banner).
 * @param est_index_of         Station → kernel slot map (−1 = not folded this
 *                             epoch ⇒ contributes no edge).
 * @param plan                 This epoch's datum plan — supplies the declared
 *                             dimension and the per-constellation gauge count d.
 * @param shared_phase_keys    Registered satellite phase-bias state keys.
 * @param registered_code_keys Registered (satellite, signal) code columns.
 * @param registered_clock_sats Registered satellite clock columns — the tier-1
 *                             gauge count d under the constraint-realized clock
 *                             datums, where @c plan.clock_pivot is empty.
 * @param datum_changed        This epoch re-referenced the datum: the ERA
 *                             boundary at which the `predicted only` identity
 *                             memory resets (the same boundary SBC re-seeds its
 *                             continuity targets on).
 */
void networkDatumGraphCensus(
    NetworkRunContext& ctx, const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log, NetworkRunTraces& traces,
    const SyncedEpoch& se, const std::vector<NetStation>& stations,
    const std::vector<std::vector<PppObservation>>& modeled,
    const std::vector<int>& est_index_of, const NetworkSbasis::Plan& plan,
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::vector<std::string>& registered_clock_sats, bool datum_changed);

} // namespace GnssProcessor
