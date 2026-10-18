#pragma once
/**
 * @file ParamInventory.hpp
 * @brief The network producer's parameter inventory: every state column of every
 *        station, and every shared state a gate withheld from it, classified into a
 *        parameter instance with a declared disposition, and the "parameters as armed"
 *        table (
 *        docs/refound/NOM_network_object_model_2026-10-08.md sections 11.2 and 13).
 *
 * ### What it answers
 *
 * The parameter registry (network/assembly/ParamRegistry.hpp) declares each parameter KIND
 * once.  It cannot say what happened to each INSTANCE: whether the receiver's band-5
 * code delay of one station is a column of its local block, held at its seed outside
 * the solve, or not observed this epoch.  A residual census was needed to find
 * that the band-5 receiver delay was dropped from the solve by a substring test whose
 * justification the goldens do not satisfy.  This file makes that a table a reader
 * finds at run start:
 *
 *     receiver IFB, GPS L5   rx_code_bias.ifb   ... held_at_seed 58 ...
 *     WARNING: receiver IFB, GPS L5: held at its first-epoch seed at 58 station(s);
 *     dropped because the satellite code biases are estimated; premise (...) does not
 *     hold on this run (...)
 *
 * ### The vocabulary
 *
 *  - @ref ParamDisposition: what the network did with one instance on one epoch (the
 *    station split with a column, or a gate with a withheld state: @c Absorbed).  Its
 *    values are exactly the count columns of the table.
 *  - @ref ParamClass: one line of the table (registry kind, @ref ParamForm, constellation,
 *    signal or band).  @ref ParamInstanceId is a class plus the subject fields a line
 *    sums over (satellite, GLONASS slot, station).
 *  - @c StateContainer: where a column sits in the station's @c StateManager, the
 *    classifier's input (not a second kind enum; see its documentation in
 *    measurement/AbsorbedSharedStates.hpp).
 *  - @ref RxIfbRule: the declared rule behind the receiver IFB drop.
 *  - @c AbsorbedReason: why a gate withheld a shared satellite state from a station, one
 *    value per gate, with its row in the reason table @ref absorbedReasonSpec (the rule,
 *    the code site, what takes the withheld state up).  @c StationAbsorbed is a station's
 *    list of the gates' notes for one epoch (measurement/AbsorbedSharedStates.hpp, kept
 *    apart so that the user-side measurement header includes no network header);
 *    @ref AbsorbedInstance is one note classified.
 *
 * ### The pieces, in the order a column passes through them
 *
 *  1. Before the split, a gate that withholds a shared satellite state (a satellite phase
 *     or code bias) from a station notes it: the station's state manager never holds the
 *     state (or the gate prunes it), so step 2 cannot see it.  @c noteAbsorbed appends
 *     the state's (container, key, reason) to the station's @c StationAbsorbed list, kept
 *     for the epoch on the station's @c MeasurementModel
 *     (@c MeasurementModel::absorbedSharedStates); step 5 classifies each note with the
 *     one classifier (step 3, @ref classifyAbsorbed).  The gates, in the order they run
 *     in an epoch:
 *      - PASS A, per station, @c MeasurementModel::ensureNetworkSharedStates (which clears
 *        the list at its start): the GLONASS code-only scope, the unmatched receiver
 *        antenna, the coverage allowlist, the legacy pivot allowlist and the CB-ADM
 *        code-census co-gate;
 *      - serial, after the screens that remove observations: the anchored-union coverage
 *        gate (@c networkBuildPhaseDatumCandidates) and the eq34 hygiene block
 *        (@c networkAnchorHygiene), through @c MeasurementModel::noteAbsorbedSharedState.
 *  2. @ref listStateColumns lists every column of a station's state manager with the
 *     container that holds it and its key; a column in no known container is listed as
 *     @c StateContainer::Unlisted with the state manager's own label.
 *  3. @ref classifyStateColumn, the one classifier, turns (container, key) into a
 *     @ref ParamInstanceId through the @c StateKeySchema parsers and the identity types
 *     of common/GnssIdentity.hpp; it returns nothing for a key it does not know, and
 *     the caller counts that column "unclassified" and prints it, never drops it.
 *  4. @c Application::networkSplitStation_ decides each column's @ref ParamDisposition
 *     (border, local block, held coordinate, held at its seed by the run's
 *     @ref RxIfbRule, all-zero H) and reports it.
 *  5. @ref recordStationParams joins the two inside the parallel produce phase (PASS B),
 *     writing only the station's own @ref StationParamSlot, and adds the station's
 *     absorbed instances from step 1 with disposition @c Absorbed and their reason.  An
 *     instance is kept only when the station still holds the column that takes it up
 *     (@ref absorberPresent); one whose observation left before the split is counted on
 *     an integrity line instead.
 *  6. @ref networkParamInventoryDrain folds the slots serially after PASS B into the
 *     epoch's and the run's @ref ParamInventoryTally; a station the produce phase
 *     rejected after its split, and a roster station that never reached its split (no
 *     usable observations, no rows, an exception), are each counted on their own line,
 *     not in the classes.
 *  7. @ref networkParamInventoryEpochTail writes the table once after the first emitted
 *     epoch and @ref networkParamInventoryRunEnd again in the run-end tail, each with a
 *     @c param_inventory record in the run manifest.
 *  8. Beside the table, @ref networkParamInventoryNoteSatellites counts on every
 *     processed epoch the satellite ids of the synced observations whose constellation
 *     the network does not model by its own rule (@ref SatIdentityCensus), and the
 *     run-end tail prints the count under the table.
 *
 * ### What is withheld but not counted as absorbed
 *
 * Three screens remove a station's observation together with its shared states: the
 * cross-station observability prefilter (a satellite seen by fewer than two stations,
 * @c Application::networkAssemblyPrefilter_), the signal-integrity withhold
 * (@c networkSignalIntegrity) and the measurement-admission withhold
 * (@c networkMeasurementAdmission).  No row is left for another parameter to absorb: PASS B
 * allocates the station's ambiguities from the observations it is given, so the ambiguity
 * of that signal leaves with the observation, both sides of the table drop together and
 * nothing is counted.  Hence, per satellite phase-bias class, the border count plus the
 * absorbed count equals the ambiguity count of the same signal (measured at N1b's gate on
 * both goldens, a three-band arm and a GPS+Galileo arm).
 *
 * Trace class: nothing here is read by the solve except the rule, which is built once per
 * run and read by the split.  Plain value types, free functions and tables:
 * no behaviour classes, no virtual dispatch.
 */

#include "common/GnssIdentity.hpp"           // System, SatId, SignalId, StationId, FrequencyBand
#include "common/ParamRef.hpp"                  // NetworkParamKind, ParamForm, ParamClass, ParamInstanceId
#include "network/assembly/ParamRegistry.hpp"   // the kind registry
#include "measurement/AbsorbedSharedStates.hpp"  // StateContainer, AbsorbedReason, StationAbsorbed

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    class GnssConfig;
    enum class FrequencyMode;   // config/GnssConfigTypes.hpp
    struct EpochMeasurement;    // common/EpochMeasurement.hpp
    class StateManager;
    struct NetworkConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;
    struct StationAssembly;
    struct SyncedEpoch;

    // =======================================================================
    // The disposition of a column
    // =======================================================================

    /**
     * @brief What the network did with one parameter instance (one state column of one
     *        station, or one shared state withheld from it) on one epoch.  The values are
     *        the count columns of the table, in print order.
     *
     * Section 11.2 of the design pre-registered a treatment @c Estimated with a route; the
     * route is folded in here as the two estimated values, so one enum carries the whole
     * decision and indexes the counts.
     */
    enum class ParamDisposition : std::uint8_t {
        EstimatedBorder = 0,  ///< Estimated: a column of the shared border (A_s).
        EstimatedLocal,       ///< Estimated: a column of the station's local block (A_r).
        HeldAtSeed,           ///< In the station's model at a fixed value (its seed), outside the solve.
        Absorbed,             ///< A shared satellite state a gate withheld from the station; another parameter of the station takes it up (@ref AbsorbedReason).
        Pinned,               ///< Held by the datum: the station coordinates.
        Dropped,              ///< Removed from the model with a stated reason (no column is given it yet).
        Unobserved,           ///< In the layout, but its H column is all zero this epoch, so left out of the block.
    };

    /// Number of @ref ParamDisposition values (the table's count columns).
    inline constexpr std::size_t kParamDispositions = 7;

    /**
     * @brief The name of a disposition: the table's column header and the JSON name.
     * @param d The disposition.
     * @return "estimated_border", "estimated_local", "held_at_seed", "absorbed",
     *         "pinned", "dropped" or "unobserved".
     */
    const char* paramDispositionName(ParamDisposition d);

    // =======================================================================
    // Parameter classes and instances
    // =======================================================================

    /**
     * @brief The human label of a class, e.g. "receiver IFB, GPS L5".
     * @param c The class.
     * @return The label.
     */
    std::string paramClassLabel(const ParamClass& c);

    // =======================================================================
    // The station's state manager, listed and classified
    // =======================================================================

    // StateContainer (where a column sits in a station's StateManager, the classifier's
    // input) is declared in measurement/AbsorbedSharedStates.hpp, beside the absorbed-state
    // record that uses it, so that the user-side measurement header includes no network
    // header.

    /**
     * @brief The name of a container, used in the unclassified labels ("<container>:<key>").
     * @param c The container.
     * @return The accessor's name in snake case ("ionosphere_states", ...; "unlisted").
     */
    const char* stateContainerName(StateContainer c);

    /**
     * @brief One column of a station's state manager.
     */
    struct StateColumn {
        std::size_t    index{0};                           ///< State index (the H column).
        StateContainer container{StateContainer::Unlisted};  ///< The container that holds it.
        std::string    key;                                ///< Its key (empty for the fixed core).
    };

    /**
     * @brief List every column of a station's state manager.
     *
     * Walks the fixed core (position, receiver clock, wet delay) and every index map the
     * network can populate; each state index in [0, @c getStateSize()) appears exactly
     * once, an index in no known container as @c Unlisted with its
     * @c StateManager::getStateLabels label, so a family added to the state manager
     * later is listed (and counted unclassified) rather than missed.
     *
     * @param sm The station's state manager.
     * @return The columns in ascending state-index order.
     */
    std::vector<StateColumn> listStateColumns(const StateManager& sm);

    /**
     * @brief The one classifier: (container, key) to a parameter instance.
     *
     * Every key is read through its @c StateKeySchema parser and the identity types; the
     * instance's station is left unset.  The refusal policy: a key is refused when any
     * of its parts fails to parse, with one exception.  The receiver code-bias
     * forms that carry no signal (DCB, IFB, slot IFB) are decided by the key's shape, and
     * a system name that is not a known constellation gives @c System::Unknown rather
     * than a refusal, so that the IFB rule names exactly the columns the former substring
     * test on "_IFB" named (StateInitializer produces no such key).  An ISC's signal is a
     * part like any other: it needs a known system.
     *
     * @param container The container the key is held in.
     * @param key       The key (ignored for the fixed core).
     * @return The instance, or @c std::nullopt for a key it does not know: the container
     *         @c Unlisted, a double-difference ambiguity key (the baseline mode's, which
     *         no network station holds), or a malformed key.
     */
    std::optional<ParamInstanceId> classifyStateColumn(StateContainer container,
                                                       std::string_view key);

    // =======================================================================
    // Absorbed instances: shared states a gate withheld from a station
    // =======================================================================

    // AbsorbedReason (one value per gate), kAbsorbedReasons, the station's record
    // StationAbsorbed and noteAbsorbed are declared in measurement/AbsorbedSharedStates.hpp:
    // the record lives on the station's MeasurementModel, whose header includes no network
    // header.  This header classifies and counts the record.

    /**
     * @brief One row of the reason table: what a reader needs to know about a gate.
     *
     * Every text was checked against the code it names; where a code comment
     * claims more than the code does, the row says what the code does.
     */
    struct AbsorbedReasonSpec {
        AbsorbedReason reason;    ///< The gate.
        const char*    name;      ///< Its name in the JSON record ("coverage_gate").
        const char*    label;     ///< Its name in the table's note column ("coverage allowlist").
        const char*    rule;      ///< When the gate withholds, with the knob that arms it.
        const char*    site;      ///< The function that holds the gate.
        /// What takes up a withheld satellite phase bias; "" when the gate never withholds one.
        const char*    by_phase;
        /// What takes up a withheld satellite code bias; "" when the gate never withholds one.
        const char*    by_code;
    };

    /**
     * @brief The reason table's row of a reason.
     * @param r The reason.
     * @return The row (static storage).
     */
    const AbsorbedReasonSpec& absorbedReasonSpec(AbsorbedReason r);

    /**
     * @brief The JSON name of a reason.
     * @param r The reason.
     * @return "glonass_code_only_scope", "coverage_gate", "allowlist", "code_census_cogate",
     *         "anchored_union_coverage" or "anchor_hygiene_block".
     */
    const char* absorbedReasonName(AbsorbedReason r);

    /**
     * @brief What takes up a withheld state of a given kind.
     * @param r    The reason.
     * @param kind The withheld state's kind (@c SatPhaseBias or @c SatCodeBias).
     * @return The reason table's @c by_phase or @c by_code text; "" for any other kind.
     */
    const char* absorbedBy(AbsorbedReason r, NetworkParamKind kind);

    /**
     * @brief One shared satellite state a gate withheld from a station this epoch, classified.
     */
    struct AbsorbedInstance {
        /// The instance: kind @c SatPhaseBias or @c SatCodeBias, its constellation, satellite
        /// and signal, as @ref classifyStateColumn reads the key the state would have had.
        /// The station is set by @ref recordStationParams.
        ParamInstanceId id{};
        AbsorbedReason  reason{AbsorbedReason::GlonassCodeOnlyScope};  ///< The gate.
    };

    /**
     * @brief Classify one withheld state a gate noted (@c noteAbsorbed): its key read by the
     *        one classifier, @ref classifyStateColumn.
     *
     * @param note The gate's note (container, key, reason).
     * @return The instance with the note's reason, or @c std::nullopt for a key the
     *         classifier refuses (the caller counts it unclassified under
     *         @ref absorbedNoteLabel, never drops it).
     */
    std::optional<AbsorbedInstance> classifyAbsorbed(const AbsorbedNote& note);

    /**
     * @brief The label of a note in the unclassified list: "<container>:<key>".
     * @param note The gate's note.
     * @return @ref stateContainerName of the container, a colon and the key.
     */
    std::string absorbedNoteLabel(const AbsorbedNote& note);

    /**
     * @brief Does the station still hold the column that takes a withheld state up?
     *
     * The test that the instance's observation reached the split: for a satellite phase bias
     * the station's float ambiguity of the same satellite and signal
     * (@c StateKeys::makeAmbiguityKey, the per-band key the network uses), for a satellite
     * code bias the satellite's clock column (the code row's shared column).
     *
     * @param sm The station's state manager after its measurement pre-pass.
     * @param a  The withheld state.
     * @return True when the column is in @p sm.
     */
    bool absorberPresent(const StateManager& sm, const AbsorbedInstance& a);

    // =======================================================================
    // The receiver IFB rule
    // =======================================================================

    /**
     * @brief The declared rule behind the receiver IFB drop of
     *        @c Application::networkSplitStation_.  Built once per run
     *        (@ref networkRxIfbRule, held on @c ctx.param_inventory.rx_ifb) and passed to
     *        the split.
     *
     * When the satellite code biases are estimated (W6), the receiver IFB columns of the
     * bands outside each constellation's clock-reference pair are left out of the
     * station's local block: kept, a non-primary receiver IFB absorbs the code
     * information that separates that band's satellite clock from its phase bias.  The
     * column stays in the station's model at the value the state initializer seeded on
     * its first epoch.  The drop rests on a premise: the GIM ionosphere weighting leaves
     * each station's ionosphere level free, so the band's receiver delay folds into it.
     * Without the weighting (or with the W6 ionosphere constraint @c none) nothing takes
     * the delay up, and the table says so.
     *
     * @c network.estimate_rx_extra_band_code_delay turns the drop off where it
     * can act (@ref rxExtraBandDelayEffective, the one predicate the loader, this rule,
     * the snapshot fingerprint and the run-end report read): @ref drop is then false,
     * @ref estimated_by_knob is true, and the governed columns take the split's ordinary
     * path (estimated in the local block unless no row informs them).  The table's note
     * and the manifest record then name the knob.  Armed where it cannot act, the rule
     * is the unarmed one.
     */
    struct RxIfbRule {
        bool             drop{false};                                ///< IFB columns are dropped on this run (network.estimate_sat_code_bias, unless @ref estimated_by_knob).
        ParamDisposition disposition{ParamDisposition::HeldAtSeed}; ///< The disposition a dropped instance gets.
        const char*      dropped_by{"network.estimate_sat_code_bias"};  ///< The knob whose arming drops the columns.
        /// The drop is turned off by @ref estimated_by on this run
        /// (@ref rxExtraBandDelayEffective): the satellite code biases are estimated (the
        /// drop would apply), the knob is armed and some active constellation tracks a band
        /// outside a two-band pair, so the governed columns are estimated.
        bool             estimated_by_knob{false};
        const char*      estimated_by{"network.estimate_rx_extra_band_code_delay"};  ///< The knob that turns the drop off.
        /// Some active constellation tracks a band outside a two-band pair under the run's
        /// frequency mode (@ref rxExtraBandTracked).  Read only to name why an armed knob is
        /// inert; it changes nothing printed on an unarmed run.
        bool             extra_band_tracked{false};
        const char*      source{"first-epoch seed, StateInitializer"};  ///< Where the held value comes from.
        /// The premise the drop rests on.
        const char*      premise{"GIM weighting leaves the station ionosphere level free to "
                                 "absorb the band's receiver delay"};
        bool             iono_weighting{false};  ///< estimation.ionosphere_weighting.enabled on this run.
        bool             w6_iono_none{false};    ///< network.w6_iono_constraint is "none" on this run.
        /// The premise holds on this run: ionosphere weighting enabled and the W6
        /// ionosphere constraint not "none".
        bool             premise_holds{false};

        /**
         * @brief Is a class governed by the rule?  The one test of it: the split's drop, the
         *        table's note, the WARNING and the manifest record all read it.
         *
         * Independent of whether the rule drops on this run.  The GLONASS per-slot IFB is
         * a different form and is not governed.
         *
         * @param c The class.
         * @return True for the receiver IFB form (kind rx_code_bias, form Ifb).
         */
        static bool governs(const ParamClass& c) {
            return c.kind == NetworkParamKind::RxCodeBias && c.form == ParamForm::Ifb;
        }

        /**
         * @brief Does the rule drop this instance on this run?
         * @param id The instance.
         * @return @ref drop and the instance's class is governed (@ref governs).
         */
        bool drops(const ParamInstanceId& id) const { return drop && governs(id.cls); }
    };

    /**
     * @brief The receiver IFB rule of this run.
     *
     * @param config The run's configuration.
     * @return The rule: @c estimated_by_knob from @ref rxExtraBandDelayEffective, @c drop
     *         from @c estimateNetworkSatCodeBias() unless @c estimated_by_knob, the
     *         premise from @c getIonosphereWeightingConfig().enabled and
     *         @c getNetworkConfig().w6_iono_constraint.
     */
    RxIfbRule networkRxIfbRule(const GnssConfig& config);

    /**
     * @brief Does some active constellation track a band outside a two-band pair under a
     *        frequency mode?  The configuration-time answer to "can a receiver IFB column
     *        exist on this run".
     *
     * A constellation counts when it is not FDMA (GLONASS never has a per-band receiver
     * IFB: @c StateInitializer::resolveReceiverBias gives it the per-slot form) and the
     * mode admits more than two of its bands (@c getAllowedBandDigits, the band map the
     * measurement model reads).  It rests on the two bands of @c FrequencyMode::Dual being
     * the constellation's clock-reference pair (GPS L1/L2, Galileo E1/E5a, BeiDou B1I/B3,
     * QZSS L1/L2), which is what the BIA products declare; a product whose clock
     * reference is another pair would leave an IFB column on a dual-band run, and the knob
     * then stays inert on it (the rule holds the column, as unarmed).  SBAS has two bands
     * in every mode.
     *
     * @param mode           @c processing.frequency_mode.
     * @param active_systems @c processing.active_systems (names as the configuration
     *                       writes them; an unknown name counts for nothing).
     * @return True when some active constellation has a third band.
     */
    bool rxExtraBandTracked(FrequencyMode mode, const std::vector<std::string>& active_systems);

    /**
     * @brief Is @c network.estimate_rx_extra_band_code_delay effective on this run?  The
     *        one predicate: the loader's interlocks and posture line, the rule
     *        (@ref RxIfbRule::estimated_by_knob), the snapshot fingerprint token and the
     *        run-end report all read it, so none of them can call an armed knob effective
     *        where another calls it inert.
     *
     * @param config The run's configuration.
     * @return The knob armed, @c estimateNetworkSatCodeBias() (otherwise nothing is
     *         dropped) and @ref rxExtraBandTracked for the run's frequency mode and active
     *         systems (otherwise no governed column exists).
     */
    bool rxExtraBandDelayEffective(const GnssConfig& config);

    /**
     * @brief One station's state and counts for the receiver IFB columns that
     *        @c network.estimate_rx_extra_band_code_delay estimates: the ON-path
     *        telemetry of standing invariants #4 and #7, and the edge state the split and
     *        the harvest need.
     *
     * A member of @c NetStation (network/driver/RunTypes.hpp), written inside the parallel
     * produce phase by the station's own split and inject, as @c eb_rows_deweighted is,
     * and by the serial harvest; summed by @ref networkRxExtraBandDelayRunEnd.  Untouched
     * unless @ref RxIfbRule::estimated_by_knob.  The split's dispositions of the governed
     * columns are not counted here: the parameter inventory owns them
     * (@c NetworkParamInventory::whole_run).
     */
    struct RxIfbStationState {
        // --- Counts over the run ---
        /// Station-epochs whose split estimated a governed column in the local block (the
        /// per-station view the zero-fold warning needs; the run's totals are the
        /// inventory's, which also leaves out station-epochs rejected after the split).
        long long est_epochs{0};
        /// Extra-band code rows excluded (zero weight, @ref rxIfbExcludedRows): the row's
        /// satellite code bias has no shared column this epoch (held by fewer than two
        /// stations and pruned, or a CB-ADM sponge), so the row carries an unmodelled
        /// satellite bias of metres.
        long long rows_unregistered{0};
        /// Station-epochs whose governed column had only such rows, so no weighted row
        /// informed it and it was left out of the block (@c ParamDisposition::Unobserved).
        long long cols_unregistered{0};
        long long reg_edges{0};    ///< Governed rows whose satellite column registered since the station's previous split: the row joins the solve (invariant #7, re-coupling edge).
        long long dereg_edges{0};  ///< Governed rows whose satellite column deregistered since the previous split: the row leaves the solve (decoupling edge).
        /// Governed columns estimated again after at least one loop epoch out of the block
        /// while a carry entry was live: the column's re-coupling edge.  The coupling row
        /// of such an entry is gauge-restored per (constellation, kind), not per signal
        /// (@c CouplingRowBuilder::tuGaugeRestore, as for the receiver DCB); this count is
        /// how often that applies.
        long long recouple{0};
        long long blk_folds{0};         ///< Governed columns folded as rows of a factorized station block by the in-block carry (scalar-screen survivors only).
        long long diag_folds{0};        ///< Governed columns folded as diagonal priors (diagonal carry route).
        long long diag_innov_skips{0};  ///< Governed columns the diagonal route's inject screen withheld.
        /// Carry entries erased at the harvest because the satellite sum-to-zero of the
        /// column's family re-seeded this epoch (no veteran satellite): the entry holds the
        /// old level of the family's null direction and the satellite side now realizes
        /// another (reset, not repair).
        long long reseed_erase{0};

        // --- Edge state ---
        /// "<sat>|<signal>" of every governed row at the station's previous split, with
        /// whether its satellite column was registered then.
        std::map<std::string, bool> row_registered;
        /// Governed key -> the code signals of its rows at the latest split that had any:
        /// the station's extra-band code family ("C5Q", "C5X").
        std::map<std::string, std::set<std::string>> signals;
        /// Governed key -> the loop epoch of its latest estimate in the local block.
        std::map<std::string, long long> last_est_epoch;
    };

    /**
     * @brief The extra-band code rows of a station that the split excludes (zero weight)
     *        because the knob estimates the receiver IFB, with the station's
     *        counts and edge state kept.
     *
     * An extra-band code row carries the satellite's extra-band code bias.  When that bias
     * is a shared column this epoch the column takes it, and the row informs the receiver
     * IFB.  When it is not (the satellite's key is held by fewer than two stations, so
     * PASS A pruned it, or CB-ADM keeps it as a station-local sponge) the row carries the
     * whole satellite bias, metres, with nothing to take it up: an estimated, carried IFB
     * would integrate a share of it and push it onto the station's other extra-band rows
     * and their ionosphere, and with the IFB term held instead the row closes the
     * station's geometry-free level direction with that bias (the ionosphere, the receiver
     * DCB and the ambiguities move with it).  Neither is information, so the row is
     * excluded: the split sets its weight to zero, and a local column that then has no
     * weighted row (the IFB itself, a sponge) is left out of the block.  A zero weight is
     * a defined case for every reader of the row weights (the block cofactor check counts
     * such a block as not checkable; the standardized residuals read zero).
     *
     * Called by @c Application::networkSplitStation_ only when
     * @ref RxIfbRule::estimated_by_knob, inside the parallel produce phase: it reads the
     * station's state manager and measurement update and writes only the station's own
     * @p s (the rows excluded, the governed columns left with no weighted row, the
     * registration edges, and the family signals).
     *
     * @param s           The station's band-5 receiver-delay state (written).
     * @param sm          The station's state manager (governed columns, satellite code-bias
     *                    columns).
     * @param em          The station's measurement update (H and the row identities
     *                    @c satellite_ids / @c signal_types).
     * @param shared_cols The H columns the split scattered into the shared border.
     * @return The data rows to exclude, ascending.
     */
    std::vector<int> rxIfbExcludedRows(RxIfbStationState& s, const StateManager& sm,
                                       const EpochMeasurement& em,
                                       const std::set<int>& shared_cols);

    /**
     * @brief Did the satellite sum-to-zero of the family a station's receiver IFB column
     *        belongs to re-seed this epoch?
     *
     * @param s               The station's state (its @c signals of @p ifb_key).
     * @param ifb_key         The governed key ("GPS_IFB5").
     * @param reseed_channels The channels whose sum-to-zero row re-seeded this epoch,
     *                        "<constellation letter>|<signal>"
     *                        (@c NetworkSbasis::CodeBiasDatumRows::reseed_channels).
     * @return True when one of the key's signals at this station is such a channel of the
     *         key's constellation.
     */
    bool rxIfbFamilyReseeded(const RxIfbStationState& s, const std::string& ifb_key,
                             const std::set<std::string>& reseed_channels);

    // =======================================================================
    // Per-station slot (written in the parallel produce phase)
    // =======================================================================

    /**
     * @brief One instance's record: the instance and what the network did with it.
     */
    struct StationParamRecord {
        ParamInstanceId  id{};                                ///< The instance (station set).
        ParamDisposition disp{ParamDisposition::Unobserved};  ///< Its disposition this epoch.
        /// The gate that withheld it: set exactly when @ref disp is @c Absorbed.
        std::optional<AbsorbedReason> reason{};
    };

    /**
     * @brief One station's inventory of one epoch.  Lives in @c StationAssembly, so the
     *        parallel produce phase writes only its own station's slot.
     */
    struct StationParamSlot {
        bool                            filled{false};     ///< The station reached the split this epoch.
        /// One per classified column with a disposition, then one per absorbed instance.
        std::vector<StationParamRecord> records;
        /// "<container>:<key>" of every column the classifier refused, and of every withheld
        /// state it refused (prefixed "absorbed ").
        std::vector<std::string>        unclassified;
        /// Classified columns the split gave no disposition (its disposition vector is
        /// shorter than the state): a defect, counted and printed, never filled in.
        long long                       no_disposition{0};
        /// Withheld states whose absorbing column the station no longer holds at the split
        /// (@ref absorberPresent false: the observation left before PASS B).  Counted and
        /// printed, not in the classes.
        long long                       absorbed_no_absorber{0};
    };

    /**
     * @brief Record a station's inventory after its split.
     *
     * Classifies every column of @p sm and pairs it with the split's disposition, then adds
     * each withheld state of @p absorbed whose absorbing column @p sm holds as a record
     * with disposition @c Absorbed and its reason.  Pure per-station work: reads @p sm,
     * @p col_disp and @p absorbed, writes only @p slot (safe inside the parallel PASS B).
     *
     * @param sm       The station's state manager.
     * @param station  The station's roster index.
     * @param col_disp The split's disposition per state index (expected size: the state
     *                 size; a shorter vector is counted in @c slot.no_disposition).
     * @param absorbed The station's withheld shared states of this epoch
     *                 (@c MeasurementModel::absorbedSharedStates).
     * @param slot     The station's slot (overwritten).
     */
    void recordStationParams(const StateManager& sm, int station,
                             const std::vector<ParamDisposition>& col_disp,
                             const StationAbsorbed& absorbed,
                             StationParamSlot& slot);

    // =======================================================================
    // Tallies and the table
    // =======================================================================

    /**
     * @brief The counts of one class.
     */
    struct ParamClassCount {
        std::array<long long, kParamDispositions> n{};  ///< Instances per disposition.
        /// Absorbed instances per reason (they sum to the @c Absorbed count).
        std::array<long long, kAbsorbedReasons> absorbed_by{};
        std::vector<char> stations;                     ///< stations[i] != 0: station i held an instance.

        /**
         * @brief The count of one disposition.
         * @param d The disposition.
         * @return Instances with that disposition.
         */
        long long count(ParamDisposition d) const { return n[static_cast<std::size_t>(d)]; }

        /**
         * @brief The absorbed count of one reason.
         * @param r The reason.
         * @return Absorbed instances withheld by that gate.
         */
        long long countAbsorbed(AbsorbedReason r) const {
            return absorbed_by[static_cast<std::size_t>(r)];
        }

        /**
         * @brief Number of distinct stations that held an instance.
         * @return The count.
         */
        long long nStations() const;
    };

    /**
     * @brief Counts per class over a set of station slots (one epoch, or a whole run).
     */
    struct ParamInventoryTally {
        std::map<ParamClass, ParamClassCount> classes;   ///< Counts per class.
        long long epochs{0};                             ///< Epochs in which a station reached the split.
        long long station_epochs{0};                     ///< Station slots counted in the classes.
        /// Station slots that reached the split but whose station the produce phase then
        /// rejected (an exception after the split, or no usable block): not counted in the
        /// classes, because the solve never saw them.
        long long station_epochs_rejected{0};
        /// Roster stations of an assembled epoch whose slot was never filled: the station did
        /// not reach its split.  The sum of the four causes below.
        long long station_epochs_unreached{0};
        /// Not assembled in PASS B: no usable observations this epoch (absent from the synced
        /// epoch, or its models refused them in PASS A).
        long long unreached_absent{0};
        long long unreached_no_rows{0};         ///< Assembled, but computeMeasurementUpdate built no row (@c StationAssembly::empty_innov).
        long long unreached_state_mismatch{0};  ///< Assembled, but H's column count differed from the state size (@c state_mismatch).
        long long unreached_exception{0};       ///< Assembled, but an exception before the split (@c threw), or no flag set.
        std::vector<char> stations;                      ///< stations[i] != 0: station i counted at least once.
        long long unclassified{0};                       ///< Columns (and withheld states) the classifier refused.
        std::map<std::string, long long> unclassified_labels;  ///< "<container>:<key>" -> columns (first 50 labels kept).
        long long no_disposition{0};                     ///< Columns the split gave no disposition (@ref StationParamSlot::no_disposition).
        /// Withheld states without their absorbing column at the split
        /// (@ref StationParamSlot::absorbed_no_absorber).
        long long absorbed_no_absorber{0};
        int    week_first{0};   ///< GPS week of the first epoch folded in.
        double tow_first{0.0};  ///< GPS time of week of the first epoch folded in (s).
        int    week_last{0};    ///< GPS week of the last epoch folded in.
        double tow_last{0.0};   ///< GPS time of week of the last epoch folded in (s).

        /**
         * @brief Fold one station's slot into the classes.
         * @param station The station's roster index.
         * @param slot    Its slot (ignored unless filled).
         */
        void addStation(int station, const StationParamSlot& slot);

        /**
         * @brief Fold another tally (counts summed, station sets united, the time span
         *        extended).  A tally with no epoch and no unreached station is ignored.
         * @param other The tally.
         */
        void merge(const ParamInventoryTally& other);

        /**
         * @brief Number of distinct stations counted.
         * @return The count.
         */
        long long nStations() const;
    };

    /**
     * @brief The "parameters as armed" table.
     *
     * A title line (the scope, the time span, the station and epoch counts), a header,
     * one line per class (label, kind and form, the count per disposition, the number of
     * stations, and the note that explains them: the rule or fixed treatment, and the
     * absorbed count per reason, e.g. "absorbed: coverage allowlist 5"), one line per
     * reason with absorbed instances (its rule, its code site and what takes the withheld
     * state up), one WARNING line per class governed by @p rule whose premise does not
     * hold with instances present, and five integrity lines: the station-epochs that did
     * not reach the split (by cause), the station-epochs rejected after the split, the
     * withheld states without their absorbing column, the columns without a disposition,
     * and the unclassified columns with up to 20 of their labels.
     *
     * @param tally The counts.
     * @param rule  The receiver IFB rule of the run.
     * @param scope The title's scope, e.g. "first emitted epoch" (the tally's time span
     *              follows it).
     * @return The lines, without trailing newlines.
     */
    std::vector<std::string> paramInventoryTable(const ParamInventoryTally& tally,
                                                 const RxIfbRule& rule,
                                                 const std::string& scope);

    /**
     * @brief The same content as one JSON object (the run manifest's @c param_inventory
     *        record): per class the counts per disposition and the non-zero absorbed counts
     *        per reason (@c absorbed_reasons, by @ref absorbedReasonName), the integrity
     *        counts, and the reason table itself (@c reasons).
     *
     * @param tally The counts.
     * @param rule  The receiver IFB rule of the run.
     * @param scope "first_emitted_epoch" or "run".
     * @return The JSON text, one line.
     */
    std::string paramInventoryJson(const ParamInventoryTally& tally, const RxIfbRule& rule,
                                   const std::string& scope);

    // =======================================================================
    // Run state and the seams the driver calls
    // =======================================================================

    /**
     * @brief The satellite ids whose constellation the network does not model by its own
     *        rule, counted over the run.
     *
     * Two readers of a satellite's constellation behave differently since step N0b on
     * inputs the goldens do not contain.  An id whose letter is outside the traits table
     * (IRNSS 'I', a malformed id) reads as @c System::Unknown, and every such id shares
     * the '?' group of the containers keyed by a constellation letter (@ref letterOfSat),
     * where each letter had a group of its own.  An SBAS id has a carrier wavelength at
     * the emit re-base, where it had none.  The census counts the distinct ids of both
     * kinds in the synced observations of every processed epoch, before any model reads
     * them, so a zero on a run states that neither path was reached (on the RINEX path the
     * parser drops an unknown letter, and @c processing.active_systems removes SBAS unless
     * it is listed).
     */
    struct SatIdentityCensus {
        long long             epochs{0};        ///< Processed epochs counted (the denominator).
        long long             records{0};       ///< Satellite records counted, summed over the stations.
        std::set<std::string> unknown_system;   ///< Distinct ids whose @ref systemOfSat is @c System::Unknown.
        std::set<std::string> sbas;             ///< Distinct SBAS ids.
    };

    /**
     * @brief The inventory's run state.  Lives on @c NetworkRunContext beside the
     *        registry (@c ctx.param_inventory).
     */
    struct NetworkParamInventory {
        /**
         * @brief Build with the run's receiver IFB rule.
         * @param rule The rule (@ref networkRxIfbRule).
         */
        explicit NetworkParamInventory(const RxIfbRule& rule) : rx_ifb(rule) {}

        const RxIfbRule     rx_ifb;                      ///< The receiver IFB rule of the run (the split reads it).
        ParamInventoryTally last_epoch;                  ///< The tally of the last epoch PASS B drained.
        ParamInventoryTally whole_run;                   ///< The tally of every drained epoch.
        bool                first_table_written{false};  ///< The first-emitted-epoch table is out.
        SatIdentityCensus   sat_ids;                     ///< The satellite ids of no modelled constellation, and of SBAS.
    };

    struct NetworkRun;              // network/driver/Run.hpp
    struct NetworkEpoch;            // network/driver/Epoch.hpp

    /**
     * @brief Count this epoch's satellite ids into @c inv.sat_ids (serial, once per
     *        processed epoch, after the admission seams and before any model reads the
     *        observations).
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings:
     *            run.ctx.param_inventory (inv).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se (se).
     *
     * The former parameters, by binding:
     *   - @p inv = run.ctx.param_inventory: The inventory.
     *   - @p se = ep.se: The synced epoch the producer processes.
     */
    void networkParamInventoryNoteSatellites(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief Fold this epoch's station slots (serial, after the parallel PASS B).
     *
     * Replaces @c inv.last_epoch with this epoch's tally and adds it to
     * @c inv.whole_run.  A slot whose station was rejected after the split
     * (@c StationAssembly::ok false, or @c threw) is counted in
     * @c station_epochs_rejected and kept out of the classes.  A station whose slot was not
     * filled never reached its split: it is counted in @c station_epochs_unreached, by
     * cause (not assembled, i.e. absent from @p present; else @c empty_innov,
     * @c state_mismatch, else an exception).
     *
     * @param inv     The inventory.
     * @param asm_res The epoch's per-station assembly slots.
     * @param present The stations PASS B assembled this epoch (index-aligned with
     *                @p asm_res; a shorter vector reads as absent).
     * @param week    GPS week of the epoch.
     * @param tow     GPS time of week of the epoch (s).
     */
    void networkParamInventoryDrain(NetworkParamInventory& inv,
                                    const std::vector<StationAssembly>& asm_res,
                                    const std::vector<bool>& present,
                                    int week, double tow);

    /**
     * @brief Write the table once, after the first emitted epoch.
     *
     * Logs @ref paramInventoryTable of the last epoch's tally (WARNING lines at warn
     * level) and appends its @c param_inventory record to the run manifest; returns at
     * once when the table is out, when @p emitted is false, or when no station was
     * counted on the last epoch.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.traces (traces). Read: run.log (log), run.nc (nc).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.emitted (emitted).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx: Run context (@c ctx.param_inventory read and marked).
     *   - @p log = run.log: The run logger.
     *   - @p traces = run.traces: Trace streams (the run manifest).
     *   - @p nc = run.nc: Network configuration (the manifest switch).
     *   - @p emitted = ep.emitted: True when this epoch reached the wire.
     */
    void networkParamInventoryEpochTail(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief Write the run's table in the run-end tail, next to the registry's model
     *        table, with its @c param_inventory record (scope "run"), and under it one
     *        line of the satellite-id census (@ref SatIdentityCensus; a warning when an
     *        id was counted).
     *
     * @param ctx    Run context (read).
     * @param log    The run logger.
     * @param traces Trace streams (the run manifest).
     * @param nc     Network configuration (the manifest switch).
     */
    void networkParamInventoryRunEnd(const NetworkRunContext& ctx,
                                     const std::shared_ptr<spdlog::logger>& log,
                                     NetworkRunTraces& traces, const NetworkConfig& nc);

    // ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: the receiver extra-band code delay and parameters as armed
     *        (NOM N1).
     *
     * The subsystem's one row of kNetworkRunEndReports.  Its sections, defined
     * beside it in ParamInventory.cpp, print in this order, each gated on its own knob:
     *   - networkRxExtraBandDelayRunEnd: the receiver extra-band code delay.
     *   - networkParamInventoryTableRunEnd: parameters as armed (NOM N1).
     *   - networkStationParamsTableRunEnd: the stations' parameter sets and the checks
     *     against their state managers (network/state/StationParamsShadow.hpp).
     *
     * @param v The run at its end (network/runend/RunEnd.hpp).
     * @return 0, or the last non-zero verdict of its sections.
     */
    int networkParamInventoryRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

    // ---- Run-end reports added after the cut (hand-written) ----

    /**
     * @brief One line of a report: its text and whether it is logged as a warning.
     */
    struct RxIfbReportLine {
        bool        warn{false};  ///< Logged at warn level (else info).
        std::string text;         ///< The line, without the "Network (receiver extra-band code delay)" prefix.
    };

    /**
     * @brief The receiver extra-band code delay report as lines: the content
     *        of @ref networkRxExtraBandDelayRunEnd, from plain inputs so that a test can
     *        build it.
     *
     * Lines, in order:
     *  1. When the rule did not turn the drop off: one warning naming why the armed knob
     *     was inert (the satellite code biases not estimated, or no band outside a
     *     two-band pair under the frequency mode), and nothing else.
     *  2. Per governed class of @p whole_run (the inventory, the one owner of the split's
     *     dispositions): the station-epochs estimated in the local block, unobserved and
     *     held at the seed, and the stations.  A warning per class with held columns (a
     *     defect with the knob effective), and one when no governed column was estimated
     *     over the run.
     *  3. The rows left out of the receiver IFB for want of a shared satellite column, the
     *     station-epochs whose column had no other row, and the registration edges.
     *  4. The carry: the in-block folds, or the diagonal folds and withholds; the
     *     re-coupling edges and the re-seed erases; a warning naming the stations with a
     *     column estimated on more than one epoch that the carry never folded.
     *  5. Per (governed key, extra-band code family) of the carried values at run end: the
     *     station count, the median (a level the satellite-side datum realizes, so not
     *     gradeable on its own, invariant #5) and the p90 and maximum of
     *     |value - median| (the between-station spread, which the datum does not move).
     *
     * @param rule          The run's receiver IFB rule.
     * @param whole_run     The inventory's run tally.
     * @param stations      Per roster station: its name and its state.
     * @param carried       (roster station, governed key) -> carried absolute value (m).
     * @param carry_on      @c network.carry_rx_code_bias.
     * @param in_block      @c network.carry_rx_code_bias_in_block.
     * @return The lines.
     */
    std::vector<RxIfbReportLine> rxExtraBandDelayReport(
        const RxIfbRule& rule, const ParamInventoryTally& whole_run,
        const std::vector<std::pair<std::string, const RxIfbStationState*>>& stations,
        const std::map<std::pair<int, std::string>, double>& carried,
        bool carry_on, bool in_block);

} // namespace GnssProcessor
