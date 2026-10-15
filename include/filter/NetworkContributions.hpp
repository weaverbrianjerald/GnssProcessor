#pragma once
/**
 * @file NetworkContributions.hpp
 * @brief Every contribution to the network producer's normal equations has a declared
 *        kind (card NOM step N2a, docs/refound/NOM_network_object_model_2026-10-08.md
 *        sections 3.4 and 17; docs/refound/NOM2_network_object_model_rows_2026-10-09.md).
 *
 * ### What it answers
 *
 * A station block is a stack of rows (data, pins, ties, coupling rows, whitened carry
 * blocks, birth rows) with diagonal priors beside it, and the shared border receives
 * constraint rows, priors in three forms, pins, gauges and DD blocks.  Until this step a
 * reader that needed to know what a row was inferred it: from a coefficient magnitude, a
 * column's label prefix, or a row's position after @c cpl_row_start (card NOM section
 * 3.4).  Here each family is declared once, with its channel, its stage relative to the
 * rank gate, its subject, its unit, the parameter kinds it touches, its weight source and
 * its builder, and every row and fold carries its kind from the site that creates it.
 * The run-start table "contributions as armed" prints every declared family, a zero
 * included, so the table is the inventory.
 *
 * ### The vocabulary
 *
 *  - @ref ContributionKind (filter/NetworkContributionKind.hpp, the half the estimator
 *    includes): one value per family, and @c Unset, the default of every tag.
 *    @ref contributionSpec gives each family its @ref ContributionSpec.
 *  - @ref RowTag: the plain tag of one row of a station block (kind, satellite, signal).
 *    @c StationAssembly holds one per row of @c A_r / @c A_s (@c row_tags), one per entry
 *    of each diagonal prior channel (@c carry_tags, @c prior_tags) and the tags of the
 *    diagonal entries the coalesce merged into another entry of the same column.
 *  - The shared folds carry their kind through the estimator: a fold site names the kind
 *    (@c NetworkEstimator::contributionKind) and the fold records it with its dimension
 *    in the estimator's per-epoch @ref SharedFoldLog.
 *  - @ref ContributionTally and @ref NetworkContributionLedger: the per-epoch and run
 *    tallies, and the "contributions as armed" table.
 *
 * ### The pieces, in the order an epoch passes through them
 *
 *  1. PASS B (parallel produce): every site that appends rows to a station block appends
 *     one tag per row in lockstep (@c networkSplitStation_'s data rows, tagged by the
 *     split itself in its weight passes, the pins, the GLONASS anchors, the SH and GIM
 *     ties, the SD-EWL rows, the coupling and whitened block rows through
 *     @c CouplingRowBuilder, the LGM birth rows), and every diagonal prior entry carries
 *     its tag through the sort and the coalesce.
 *  2. The end of PASS B (serial): @ref networkContributionsStationDrain counts the rows
 *     and entries of the stations the estimator took, per kind, and checks every
 *     assembled station's tags (@ref checkStationTags: present, set, in place, and in
 *     agreement with the builders' own counters).
 *  3. The solve glue: every shared fold names its kind before the call; a within-epoch
 *     RAIM replay re-folds on a fresh arrowhead under the estimator's replay flag, and
 *     its re-folds are counted apart, so the table describes the primary build.
 *  4. The epoch tail (@ref networkContributionsEpochTail, the driver's one call): adds
 *     the epoch's shared folds, the clock pivots of the last solve and the gauges,
 *     merges the epoch into the run and writes the table once after the first emitted
 *     epoch; @ref networkContributionsRunEnd writes the run's table in the teardown tail.
 *
 * The readers (card NOM step N2b): every reader that asks "what is this row" reads the
 * tag through filter/NetworkRowClassify (@c rowClassOfKind, @c classifyRow,
 * @c raimPoolAdmitsKind): the prefit and redundancy censuses and their riders, the
 * post-fit row trace and the post-fit sigma0, the gross line-of-sight screen, the station
 * block audit, the clock level read, both RAIM pools and the RHS attribution, which also
 * names each diagonal prior entry and each shared fold by its kind.  Plain value types,
 * free functions and tables (card NOM section 10): no behaviour classes, no virtual
 * dispatch.
 */

#include "common/GnssIdentity.hpp"              // SatId, SignalId, System
#include "filter/NetworkContributionKind.hpp"   // ContributionKind, SharedFoldLog

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    // filter/NetworkParamRegistry.hpp declares it; that header includes the estimator,
    // which includes filter/NetworkContributionKind.hpp, so the scoped enum is declared
    // opaquely here (its fixed underlying type makes it complete).
    enum class NetworkParamKind : int;

    class NetworkEstimator;
    struct NetworkConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;
    struct StationAssembly;

    // =======================================================================
    // The declarations
    // =======================================================================

    /**
     * @brief The channel a contribution enters the normal equations through.
     */
    enum class ContributionChannel : std::uint8_t {
        StationRows = 0,     ///< Rows of a station block (A_s / A_r), folded by @c addStation.
        StationPriorPrePd,   ///< The station prior folded with the block, before its PD test (@c StationPrior of @c addStation).
        StationPriorPostPd,  ///< The diagonal station prior folded after the PD test (@c addStationPrior).
        SharedRows,          ///< Constraint rows on the shared border (@c addSharedConstraintRows).
        SharedPrior,         ///< Diagonal (or gauge-free) priors on the shared border.
        SharedDensePrior,    ///< Dense information blocks and walk factors on the shared border.
        Pins,                ///< Exact pins applied inside the solve.
        Gauges,              ///< Post-reduce gauges folded onto the reduced system.
        DdBlocks,            ///< Post-reduce DD prior blocks.
    };

    /// Number of @ref ContributionChannel values.
    inline constexpr std::size_t kContributionChannels = 9;

    /**
     * @brief When a contribution reaches the reduced system, relative to the rank gate
     *        (the null-space test of the reduced shared normals before the solve).
     */
    enum class ContributionStage : std::uint8_t {
        BeforeGate = 0,  ///< In the reduced normals the rank gate inspects.
        AtGate,          ///< Folded by the rank gate's own response (drop-and-heal).
        AfterGate,       ///< Folded after the gate passed, before the solve.
        InSolve,         ///< Applied inside the solve (pins; the DD blocks), not seen by the gate.
        AfterSolve,      ///< Folded after the first solve, before a re-solve (the clock-anchor guard).
    };

    /**
     * @brief What one instance of a contribution is about: the identity its tag carries.
     *
     * A station block's tags name the satellite and signal; the station is the block.
     */
    enum class ContributionSubject : std::uint8_t {
        StationLosSignal = 0,  ///< One signal on one line of sight of one station (a data row, an ambiguity).
        StationLos,            ///< One line of sight of one station, no signal (a slant ionosphere, a phase colour).
        Station,               ///< One station-wide column or its signal (wet delay, receiver clock, receiver bias).
        Satellite,             ///< One satellite on the shared border (a satellite clock).
        SatelliteSignal,       ///< One (satellite, signal) of the shared border (a code or phase bias).
        None,                  ///< Network-wide (SH coefficients, a whole datum, a DD block).
    };

    /**
     * @brief What the table's count column counts for a kind: the unit of
     *        @c ContributionCount::count.
     */
    enum class ContributionUnit : std::uint8_t {
        Rows = 0,  ///< Rows of a station block, or constraint rows handed to a shared fold.
        Entries,   ///< Entries of a diagonal station prior channel (a merged entry counts under its own kind).
        Keys,      ///< Border keys handed to a shared prior fold (a key absent from this epoch's layout is skipped by the fold, not by the count).
        Walks,     ///< Clone walk factors handed to a fold (one per border key and its clone).
        Pins,      ///< Clock pivot pins of the last primary solve.
        Gauges,    ///< Sum-to-zero phase-bias gauges set for the solve (the estimator's gauge kernel).
        Blocks,    ///< DD prior blocks set for the solve.
    };

    /// A set of @c NetworkParamKind values, one bit per kind (bit = the kind's value).
    using ParamKindSet = std::uint32_t;

    /**
     * @brief The declaration of one contribution family: one row of the table.
     *
     * Every text was checked against the code it names (card NOM N2a), every knob path
     * against the loader.
     */
    struct ContributionSpec {
        ContributionKind    kind;            ///< The family.
        const char*         name;            ///< Its name in the table and the JSON record ("data_code").
        ContributionChannel channel;         ///< Where it enters the normal equations.
        ContributionStage   stage;           ///< When, relative to the rank gate.
        ContributionSubject subject;         ///< What one instance is about.
        ContributionUnit    unit;            ///< What the count column counts.
        ParamKindSet        params;          ///< The parameter kinds its rows or folds touch.
        const char*         weight;          ///< The weight source: the knob or the named constant.
        const char*         builder;         ///< The function (and file) that builds it.
        bool                is_observation;  ///< True for rows of this epoch's observations.
        const char*         note;            ///< How the count reads where the unit alone does not say (printed after the builder), or null.
    };

    /**
     * @brief The declaration table: one entry per kind, in enum order (checked at compile
     *        time).
     * @return The table (static storage).
     */
    const std::array<ContributionSpec, kContributionKinds>& contributionSpecs();

    /**
     * @brief The declaration of a kind.
     * @param k The kind (@c Unset, or a value out of range, returns the first entry).
     * @return Its table entry (static storage).
     */
    const ContributionSpec& contributionSpec(ContributionKind k);

    /**
     * @brief The name of a kind.
     * @param k The kind.
     * @return @c contributionSpec(k).name, e.g. "data_code"; "unset" for @c Unset.
     */
    const char* contributionKindName(ContributionKind k);

    /**
     * @brief The name of a channel.
     * @param c The channel.
     * @return "station_rows", "station_prior_pre_pd", "station_prior_post_pd", "shared_rows",
     *         "shared_prior", "shared_dense_prior", "pins", "gauges" or "dd_blocks".
     */
    const char* contributionChannelName(ContributionChannel c);

    /**
     * @brief The name of a stage.
     * @param s The stage.
     * @return "before_gate", "at_gate", "after_gate", "in_solve" or "after_solve".
     */
    const char* contributionStageName(ContributionStage s);

    /**
     * @brief The name of a subject.
     * @param s The subject.
     * @return "station_los_signal", "station_los", "station", "satellite",
     *         "satellite_signal" or "none".
     */
    const char* contributionSubjectName(ContributionSubject s);

    /**
     * @brief The name of a unit.
     * @param u The unit.
     * @return "rows", "entries", "keys", "walks", "pins", "gauges" or "blocks".
     */
    const char* contributionUnitName(ContributionUnit u);

    /**
     * @brief Does a family touch a parameter kind?
     * @param spec The family's declaration.
     * @param k    The parameter kind.
     * @return True when @p k is in @c spec.params.
     */
    bool contributionTouches(const ContributionSpec& spec, NetworkParamKind k);

    /**
     * @brief Is a kind one that @c CouplingRowBuilder::appendCouplingRows appends, so that
     *        its rows sit in a station block's coupling region (rows @c cpl_row_start to
     *        @c cpl_row_start + @c cpl_row_local_cols.size())?
     * @param k The kind.
     * @return True for @c CouplingScalar and the six @c Block*Carry kinds.
     */
    bool contributionInCouplingRegion(ContributionKind k);

    /**
     * @brief The parameter kinds of a set, by their registry names.
     * @param set The set.
     * @return The names in registry order, comma-separated ("sat_clock,ambiguity"); "" for
     *         an empty set.
     */
    std::string paramKindSetText(ParamKindSet set);

    // =======================================================================
    // Row tags
    // =======================================================================

    /**
     * @brief The tag of one row of a station block, or of one diagonal prior entry.
     *
     * The subject the family declares: the satellite and the signal where it has them,
     * default (invalid) values otherwise.  A whitened row carries the identity of the
     * input row on the diagonal of L⁻¹ (its primary row).  A default tag is @c Unset, so a
     * row no site named is caught by the tag check rather than read as a data row.
     */
    struct RowTag {
        ContributionKind kind{ContributionKind::Unset};  ///< The family.
        SatId            sat{};                          ///< The satellite, where the subject has one.
        SignalId         signal{};                       ///< The signal, where the subject has one.
    };

    /**
     * @brief A tag from the string identities the builders hold.
     *
     * @param kind The family.
     * @param sat  A RINEX satellite id ("G05"), or empty for none.
     * @param code A RINEX observation code ("L1W", "C1C"), read under the satellite's
     *             constellation, or empty for none.
     * @return The tag; a satellite or code that does not parse leaves that field default.
     */
    RowTag makeRowTag(ContributionKind kind, std::string_view sat, std::string_view code = {});

    /**
     * @brief A tag whose signal belongs to a constellation rather than a satellite (a
     *        receiver phase bias "GPS_RPB_L2W").
     *
     * @param kind The family.
     * @param sys  The constellation.
     * @param code The observation code, or empty for none.
     * @return The tag with no satellite.
     */
    RowTag makeRowTag(ContributionKind kind, System sys, std::string_view code);

    /**
     * @brief The signal of a receiver phase-bias key ("GPS_RPB_L2W",
     *        @c StateKeys::makeReceiverPhaseBiasKey).
     * @param key The key.
     * @return The signal; default (invalid) for a key that does not parse.
     */
    SignalId rxPhaseBiasSignal(std::string_view key);

    /**
     * @brief The signal of a receiver code-bias key: the signal of an inter-signal state
     *        ("GPS_ISC_C1C"); none for a DCB or an IFB key, whose subject is a band pair or
     *        a band, not a signal.
     * @param key The key.
     * @return The signal; default (invalid) when the key names none.
     */
    SignalId rxCodeBiasSignal(std::string_view key);

    /**
     * @brief The tag of a row or prior on a receiver phase bias, from its key.
     * @param kind The family.
     * @param key  The receiver phase-bias key.
     * @return The tag with @ref rxPhaseBiasSignal of the key and no satellite.
     */
    RowTag rxPhaseBiasTag(ContributionKind kind, std::string_view key);

    /**
     * @brief The tag of a row or prior on a receiver code bias, from its key.
     * @param kind The family.
     * @param key  The receiver code-bias key.
     * @return The tag with @ref rxCodeBiasSignal of the key and no satellite.
     */
    RowTag rxCodeBiasTag(ContributionKind kind, std::string_view key);

    /**
     * @brief Tag the data rows of a station block before the split's weight passes: one
     *        tag per row of the measurement update.
     *
     * A phase row is @c DataPhase and any other row @c DataCode, with the row's satellite
     * and signal.  @c networkSplitStation_ calls this, then changes a row's kind in the
     * weight pass that decides it (@c DataCodeCompanion in the second-code pass,
     * @c DataPhaseSecond in the second-phase pass), so a tag and a weight cannot disagree.
     * A row past the end of the identity vectors (an asserted invariant says there is
     * none) is tagged @c DataCode with no identity, so the tag vector stays aligned.
     *
     * @param tags The station's tag vector; cleared, then filled with @p m tags.
     * @param m    Rows of the measurement update (@c em.H.rows()).
     * @param sats The rows' satellites (@c em.satellite_ids).
     * @param sigs The rows' observation codes (@c em.signal_types).
     */
    void tagDataRows(std::vector<RowTag>& tags, Eigen::Index m,
                     const std::vector<std::string>& sats,
                     const std::vector<std::string>& sigs);

    /**
     * @brief Rows of a station block without a tag: the gap between the block's rows and
     *        its row tags, and between each diagonal prior channel and its tags.
     * @param R The station's assembly.
     * @return |rows(A_r) − row tags| + |carry entries − carry tags| + |prior entries − prior
     *         tags|; 0 when every row and entry is tagged.
     */
    long long untaggedRows(const StationAssembly& R);

    /**
     * @brief The result of @ref checkStationTags on one assembled station block.  Each
     *        count is 0 on a block whose tags are complete and right.
     */
    struct StationTagCheck {
        long long untagged{0};      ///< @ref untaggedRows.
        long long unset{0};         ///< Tags (rows and diagonal entries) whose kind is @c Unset or out of range.
        long long misplaced{0};     ///< Tags whose kind does not belong where the tag sits (see @ref checkStationTags).
        long long mismatches{0};    ///< Builder counters that disagree with the tags' per-kind counts.
        long long unclassified{0};  ///< Rows of the block the row classifier files as @c obs_other (card NOM N2b: no tag, an unset kind, or a kind that is not a station row).
    };

    /**
     * @brief Check one assembled station block's tags against its rows and its builders.
     *
     * Four checks, each counted on its own integrity line of the table:
     *  - present: @ref untaggedRows;
     *  - set: no tag is @c Unset;
     *  - in place: the split's data rows (rows @c 0 to @c n_data_rows) carry the
     *    observation kinds and no other row does; the coupling region (rows
     *    @c cpl_row_start to @c cpl_row_start + @c cpl_row_local_cols.size()) carries the
     *    kinds of @ref contributionInCouplingRegion and no other row does; every tag's kind
     *    belongs to the channel of its vector (@c row_tags: station rows; @c carry_tags and
     *    @c carry_merged: the post-PD prior; @c prior_tags and @c prior_merged: the pre-PD
     *    prior);
     *  - counted: the tags' per-kind counts (rows and entries built, folded or not) agree
     *    with the builders' own counters: @c ambiguity_pin + @c eq34_rpb_pin with @c np,
     *    @c phase_colour_birth with @c pc_birth_rows, @c phase_colour_backstop with
     *    @c pc_backstop_rows, the six @c block_*_carry kinds with @c pl_blk_rows, @c sd_ewl
     *    with @c sd_ewl_rows (the SD-EWL builder's @c rows_built this station-epoch),
     *    @c prior_ambiguity_oracle_pin with @c opin_rows, and @c prior_rx_cb_datum +
     *    @c prior_rx_cb_datum_pre_pd with @c pl_cb_datum_rows (one mismatch per counter
     *    that disagrees).
     *
     * It also counts the rows a reader would file as unclassified (@c obs_other of
     * filter/NetworkRowClassify @c rowClassOfKind): a row past the tags, an unset kind, or
     * a kind that is not a station row (card NOM step N2b; 0 expected).
     *
     * @param R The station's assembly (assembled: @c ok and not @c threw).
     * @return The counts.
     */
    StationTagCheck checkStationTags(const StationAssembly& R);

    /**
     * @brief The outcome of the GIM ionosphere row builder on one station-epoch (card NOM
     *        section 16, suspect 4: the builder ran silently inert on the golden).
     */
    enum class GimRowsOutcome : std::uint8_t {
        NotCalled = 0,  ///< The builder did not run for the station.
        NoGim,          ///< No GIM is loaded (the W6-none setup never builds one).
        WeightingOff,   ///< estimation.ionosphere_weighting.enable is off.
        NoRows,         ///< Ran, and no line of sight qualified.
        Appended,       ///< Appended rows.
    };

    // =======================================================================
    // Tallies, the table and the seams
    // =======================================================================

    /**
     * @brief One kind's count: the count in the kind's unit (@ref ContributionUnit) and the
     *        folds (station blocks carrying the kind, or shared fold calls).
     */
    struct ContributionCount {
        long long count{0};  ///< Rows, entries, keys, walks, pins, gauges or blocks (the kind's unit).
        long long folds{0};  ///< Station blocks or fold calls.
    };

    /**
     * @brief The contributions of one epoch, or summed over a run.
     */
    struct ContributionTally {
        std::array<ContributionCount, kContributionKinds> kinds{};  ///< Per kind, in enum order.
        long long epochs{0};                  ///< Epochs tallied (reached the epoch tail).
        long long station_blocks{0};          ///< Station blocks the estimator took.
        long long station_epochs_checked{0};  ///< Assembled station blocks whose tags were checked.
        long long untagged_rows{0};           ///< @c StationTagCheck::untagged summed over them.
        long long unset_tags{0};              ///< @c StationTagCheck::unset summed over them.
        long long misplaced_rows{0};          ///< @c StationTagCheck::misplaced summed over them.
        long long counter_mismatches{0};      ///< @c StationTagCheck::mismatches summed over them.
        long long unclassified_rows{0};       ///< @c StationTagCheck::unclassified summed over them (card NOM N2b).
        long long coalesced_entries{0};       ///< Diagonal prior entries merged into another entry of the same column.
        long long folds_without_kind{0};      ///< Shared folds whose site named no kind (@c Unset).
        long long replay_builds{0};           ///< Within-epoch replay builds.
        long long replay_folds{0};            ///< Their shared re-folds (not in @ref kinds).
        long long replay_dim{0};              ///< Their summed dimension.
        long long gim_calls{0};               ///< Station-epochs the GIM row builder ran on.
        long long gim_no_gim{0};              ///< Of those, with no GIM loaded.
        long long gim_weighting_off{0};       ///< Of those, with the ionosphere weighting off.
        int       week_first{0};              ///< GPS week of the first epoch.
        double    tow_first{0.0};             ///< Time of week of the first epoch (s).
        int       week_last{0};               ///< GPS week of the last epoch.
        double    tow_last{0.0};              ///< Time of week of the last epoch (s).

        /**
         * @brief Add another tally (the epoch into the run).
         * @param other The tally to add; its first and last times extend this one's range.
         */
        void merge(const ContributionTally& other);
    };

    /**
     * @brief The run's contribution state, held in the run context.
     */
    struct NetworkContributionLedger {
        ContributionTally pending{};      ///< This epoch's station side, from the end of PASS B.
        ContributionTally last_epoch{};   ///< The last epoch tallied.
        ContributionTally whole_run{};    ///< Every epoch tallied.
        // The tag checks over every assembled epoch, tallied or not (an epoch withheld after
        // PASS B is still checked).
        long long untagged_rows_all{0};   ///< @c StationTagCheck::untagged.
        long long unset_tags_all{0};      ///< @c StationTagCheck::unset.
        long long misplaced_rows_all{0};  ///< @c StationTagCheck::misplaced.
        long long mismatches_all{0};      ///< @c StationTagCheck::mismatches.
        long long unclassified_all{0};    ///< @c StationTagCheck::unclassified (card NOM N2b).
        long long checked_all{0};         ///< Station blocks checked.
        bool      first_table_written{false};  ///< The first-emitted-epoch table is out.
    };

    /**
     * @brief The station side of an epoch, at the end of PASS B (serial): the rows and
     *        prior entries of every station the estimator took, per kind, and the tag
     *        check of every assembled station.
     *
     * The counting rules:
     *  - a station assembled (@c ok, not @c threw) has its tags checked
     *    (@ref checkStationTags), taken or not;
     *  - a station the estimator took (an estimator index >= 0) has its rows counted, and
     *    its pre-PD prior entries with them (they fold in @c addStation);
     *  - its post-PD entries count only when the consume folded them
     *    (@c StationAssembly::carry_prior_folded);
     *  - an entry the coalesce merged into another entry of the same column counts under
     *    its own kind, with its channel's entries, and on the merged-entries line;
     *  - a kind's fold count is one per taken station whose block carries it;
     *  - the GIM builder's outcome counts for every station it ran on, assembled or not.
     *
     * Writes @c led.pending and the run-wide check counters, and clears this epoch's replay
     * re-alignment counters and tags.
     *
     * @param led          The ledger.
     * @param asm_res      The epoch's station assemblies.
     * @param est_index_of Each station's estimator index (-1: not taken).
     * @param week         GPS week of the epoch.
     * @param tow          GPS time of week (s).
     */
    void networkContributionsStationDrain(NetworkContributionLedger& led,
                                          const std::vector<StationAssembly>& asm_res,
                                          const std::vector<int>& est_index_of,
                                          int week, double tow);

    struct NetworkRun;              // app/NetworkRun.hpp (card NOM3 step EP1)
    struct NetworkEpoch;            // app/NetworkEpoch.hpp (card NOM3 step EP1)

    /**
     * @brief The epoch tail: the shared folds of the epoch's primary build, merged with the
     *        station side into the epoch's tally and the run's; the table and its manifest
     *        record once, after the first emitted epoch.
     *
     * Epoch form (card NOM3 step EP1; generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (app/NetworkRun.hpp). Written through non-const bindings: run.ctx (ctx),
     *            run.traces (traces). Read: run.engines.est (est), run.log (log), run.nc (nc).
     * @param ep The epoch (app/NetworkEpoch.hpp), read only. Bindings: ep.emitted (emitted).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx: Run context (@c ctx.contributions).
     *   - @p est = run.engines.est: The estimator (its @ref SharedFoldLog of this epoch).
     *   - @p log = run.log: The run logger.
     *   - @p traces = run.traces: Trace streams (the run manifest).
     *   - @p nc = run.nc: Network configuration (the manifest switch).
     *   - @p emitted = ep.emitted: True when this epoch reached the wire.
     */
    void networkContributionsEpochTail(NetworkRun& run, const NetworkEpoch& ep);

    /**
     * @brief The run's table and its manifest record (scope "run"), in the teardown tail.
     * @param ctx    Run context (read).
     * @param log    The run logger.
     * @param traces Trace streams (the run manifest).
     * @param nc     Network configuration (the manifest switch).
     */
    void networkContributionsRunEnd(const NetworkRunContext& ctx,
                                    const std::shared_ptr<spdlog::logger>& log,
                                    NetworkRunTraces& traces, const NetworkConfig& nc);

    /**
     * @brief The "contributions as armed" table: a header line, the column line, one line
     *        per kind (every declared kind, a zero included, with its unit, its weight
     *        source, its builder and its note), the GIM note, the integrity lines.
     *
     * @param epoch The epoch's tally (the first emitted epoch, or the last tallied one).
     * @param run   The run's tally (so far, or whole).
     * @param scope The scope named in the header ("first emitted epoch", "run").
     * @return The lines, without a prefix.
     */
    std::vector<std::string> contributionsTable(const ContributionTally& epoch,
                                                const ContributionTally& run,
                                                const std::string& scope);

    /**
     * @brief The table's run-manifest record (@c "record":"contributions").
     * @param epoch The epoch's tally.
     * @param run   The run's tally.
     * @param scope "first_emitted_epoch" or "run".
     * @return One JSON object.
     */
    std::string contributionsJson(const ContributionTally& epoch, const ContributionTally& run,
                                  const std::string& scope);

    // ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

    struct NetworkRunEndView;

    /**
     * @brief Run-end report: contributions as armed (NOM N2a).
     *
     * The table's row for "contributions as armed" (card NOM N2a) and its manifest record;
     * the report is the overload above.
     *
     * Gate: none of its own: the row calls the report above on every run, as the teardown
     * did, and that report applies its own.
     *
     * @param v The run at its end (filter/NetworkRunEnd.hpp).
     * @return 0 (this report does not set the exit code).
     */
    int networkContributionsRunEnd(const NetworkRunEndView& v);

    // ---- End of the run-end reports ----

} // namespace GnssProcessor
