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
 * column's label prefix, a row's position after @c cpl_row_start (seven inference
 * schemes, card NOM section 16).  Here each family is declared once, with its channel,
 * its stage relative to the rank gate, its subject, the parameter kinds it touches, its
 * weight source and its builder, and every row and fold carries its kind from the site
 * that creates it.
 *
 * ### The vocabulary
 *
 *  - @ref ContributionKind: one value per family (57), grouped as card NOM section 17
 *    lists them; @ref contributionSpec gives each its @ref ContributionSpec.
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
 *     one tag per row in lockstep (@c networkSplitStation_'s data rows through
 *     @ref tagDataRows, the pins, the GLONASS anchors, the SH and GIM ties, the SD-EWL
 *     rows, the coupling and whitened block rows through @c CouplingRowBuilder, the LGM
 *     birth rows), and every diagonal prior entry carries its tag through the sort and
 *     the coalesce.
 *  2. The end of PASS B (serial): @ref networkContributionsStationDrain counts the rows
 *     and entries of the stations the estimator took, per kind, and checks every
 *     assembled station's tag vectors against its rows (the integrity line).
 *  3. The solve glue: every shared fold names its kind before the call; a within-epoch
 *     RAIM replay re-folds on a fresh arrowhead under the estimator's replay flag, and
 *     its re-folds are counted apart, so the table describes the primary build.
 *  4. The epoch tail (@ref networkContributionsEpochTail, the driver's one call): adds
 *     the epoch's shared folds, the clock pivots of the last solve and the gauges,
 *     merges the epoch into the run and writes the table once after the first emitted
 *     epoch; @ref networkContributionsRunEnd writes the run's table in the teardown tail.
 *
 * Trace class: no reader takes a decision from a tag in this step (N2b moves the readers
 * onto them).  Plain value types, free functions and tables (card NOM section 10): no
 * behaviour classes, no virtual dispatch.
 */

#include "common/GnssIdentity.hpp"   // SatId, SignalId, System

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

    // filter/NetworkParamRegistry.hpp declares it; that header includes the estimator,
    // which includes this one, so the scoped enum is declared opaquely here (its fixed
    // underlying type makes it complete).
    enum class NetworkParamKind : int;

    class NetworkEstimator;
    class StateManager;
    struct CplRowRaw;
    struct NetworkConfig;
    struct NetworkRunContext;
    struct NetworkRunTraces;
    struct StationAssembly;

    // =======================================================================
    // The kinds and their declarations
    // =======================================================================

    /**
     * @brief One value per family of contribution to the normal equations, in the order
     *        of the declaration table (card NOM section 17's grouping).
     *
     * Against the inventory of card NOM section 16 (table B, 32 families): no family is
     * merged; these are split, one kind per value a reader has to tell apart:
     *  - B5/B6 (pins): the phase-datum ambiguity pins, and the eq34 receiver phase-bias
     *    pin, rank floor and time link (four kinds);
     *  - B7 (GLONASS anchors): the per-slot IFB anchors and the slant-ionosphere anchors;
     *  - B11 (coupling rows): the scalar rows (card 1) and the jointly whitened rows
     *    (card 1b);
     *  - B12 (whitened local block): one kind per family of the block's primary column;
     *  - B13 (LGM rows): the birth rows and the backstop rows;
     *  - B14 (diagonal local priors): one kind per family, and B15 the receiver DCB
     *    datum on the pre-PD channel apart from the same rows on the post-PD channel;
     *  - B16 (W6 code-bias datum stack): one kind per row type (eight);
     *  - B22 to B30 and B25 (the border carry in its diagonal, gauge-free and dense
     *    forms; the clones' isolated priors and their walks; the shared history).
     * The clone path's dense fold (the border carry with a clone standing in for a
     * clock) is the dense border carry's kind: the family is the carried border, the
     * clone only moves which column a clock entry lands on.
     */
    enum class ContributionKind : std::uint8_t {
        // --- Station data rows -------------------------------------------------------
        DataCode = 0,          ///< Code rows of the latched code signal.
        DataCodeCompanion,     ///< Code rows of a second code channel through a receiver inter-signal state (card SCC).
        DataPhase,             ///< Phase rows of the latched phase signal.
        DataPhaseSecond,       ///< Phase rows of a second phase channel (card SID phase 6a).
        // --- Station constraint rows -------------------------------------------------
        AmbiguityPin,          ///< Phase-datum pins on the station's anchored ambiguities (legacy pivot, prioritized, eq34 tree edges).
        Eq34RpbPin,            ///< The eq34 datum receiver's receiver phase-bias pin.
        Eq34RpbRankFloor,      ///< The eq34 zero-mean receiver phase-bias rank floor.
        Eq34RpbTimeLink,       ///< The eq34 receiver phase-bias time link toward the carried value.
        GloIfbAnchor,          ///< Zero-mean anchor rows on the GLONASS per-slot receiver IFB columns.
        GloIonoAnchor,         ///< Zero-mean anchor rows on the GLONASS slant-ionosphere columns.
        ShIonoTie,             ///< Slant-ionosphere to SH-VTEC tie rows (whitened per constellation).
        GimIono,               ///< External GIM slant-ionosphere rows (whitened per constellation).
        SdEwl,                 ///< Between-satellite single-differenced EWL rows (whitened per constellation).
        // --- Station block rows ------------------------------------------------------
        CouplingScalar,        ///< A carried local column's conditional coupling row, +1 on the column (card 1).
        CouplingJoint,         ///< The station's coupling rows whitened jointly (card 1b).
        BlockAmbiguity,        ///< Whitened local block row whose primary column is an ambiguity (F3).
        BlockZwd,              ///< Whitened local block row whose primary column is the wet delay (B9-in-block).
        BlockIono,             ///< Whitened local block row whose primary column is a slant ionosphere (I-in-block).
        BlockRxClock,          ///< Whitened local block row whose primary column is the receiver clock or an ISB (RX-CLK-in-block).
        BlockRxCodeBias,       ///< Whitened local block row whose primary column is a receiver code bias (RXCB-IN-BLOCK).
        BlockPhaseColour,      ///< Whitened local block row whose primary column is a phase colour (card LGM).
        PhaseColourBirth,      ///< The stationary prior row of a phase-colour column with no live carry (card LGM).
        PhaseColourBackstop,   ///< The stationary prior row of a carried phase-colour column whose carry did not fold (card LGM).
        // --- Station priors (diagonal channels) --------------------------------------
        PriorAmbiguity,        ///< Diagonal carried prior on an ambiguity (leveling carry, diagonal route).
        PriorZwd,              ///< Diagonal carried prior on the wet delay (B9).
        PriorIono,             ///< Diagonal carried prior on a slant ionosphere (QUEUE 8b).
        PriorRxCodeBias,       ///< Diagonal carried prior on a receiver code bias (rx-CB carry).
        PriorOraclePin,        ///< Simulation-only oracle ambiguity pin (diagonal).
        PriorRxcbDatum,        ///< Zero-mean receiver DCB datum on the post-PD channel (RXCB-DATUM, pre-PD routing off).
        PriorRxcbDatumPrePd,   ///< Zero-mean receiver DCB datum on the pre-PD channel (RXCB-DATUM, network.station_prior_before_pd).
        // --- Shared rows -------------------------------------------------------------
        CbIfPin,               ///< W6 code-bias datum: the per-satellite ionosphere-free pin row.
        CbSatGfPair,           ///< W6 code-bias datum: the per-satellite geometry-free pair row (sat_gf_datum).
        CbExtraBandHeld,       ///< W6 code-bias datum: an extra-band column held at its own level (B5D).
        CbExtraGf,             ///< W6 code-bias datum: an extra signal's geometry-free row (ISC asserted zero, RF-0d).
        CbGfPin,               ///< W6 code-bias datum: the slant posture's geometry-free pin row.
        CbFamilyGf,            ///< W6 code-bias datum: a secondary family's geometry-free row (FAMILY-GF).
        CbIfLevel,             ///< W6 code-bias datum: the per-constellation ionosphere-free level row (RF-0e e1).
        CbSumToZero,           ///< W6 code-bias datum: the (constellation, signal) sum-to-zero row.
        ClockConstraintRows,   ///< Constraint-realized clock datum rows (sum-to-zero or broadcast-anchored).
        SbasisCompletion,      ///< S-basis completion rows on the directions the declared datum misses (SBC).
        // --- Shared priors -----------------------------------------------------------
        CbKaula,               ///< Loose physical-bound prior on the satellite code biases (card KP).
        PbHeal,                ///< Drop-and-heal prior on a near-null phase-bias direction (the rank gate's response).
        ShPrior,               ///< Carried SH-VTEC prior.
        PhaseHold,             ///< Phase-bias hold prior (ensemble datum).
        PinBridge,             ///< eq34 pin bridge on an unpinned key (reattach hygiene).
        BorderCarryDiagonal,   ///< The border carry, diagonal form (D2, and the dense fold's leftovers).
        BorderCarryGaugeFree,  ///< The border carry, diagonal with the clock common mode downdated (card Q3 gauge-free fold).
        BorderCarryDense,      ///< The border carry, dense information block (C1; the clone path's stand-in block included).
        Q3Refold,              ///< The clock-anchor guard's withdrawals and refolds before its re-solve (card Q3).
        ClonePrior,            ///< Isolated priors on idle clones (card SCL / BCL).
        CloneWalk,             ///< Clone walk factors (card SCL / PFQ).
        SharedHistory,         ///< N(0, 1) priors on the shared-history columns (card SCL2 X2).
        ClockOscillator,       ///< Two-state clock-model prior on the satellite clocks.
        ColdClockAnchor,       ///< A-priori satellite-clock anchor (cold-start conditioning).
        // --- Pins, gauges, DD blocks -------------------------------------------------
        ClockPivotPin,         ///< Per-constellation clock pivot pins (Dirichlet, in the solve).
        EnsemblePhaseGauge,    ///< Ensemble sum-to-zero phase-bias gauges (fix A).
        DdPriorBlock,          ///< Card-4 DD prior blocks (carry_invariant).
    };

    /// Number of @ref ContributionKind values (the table's size).
    inline constexpr std::size_t kContributionKinds = 57;

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

    /// A set of @c NetworkParamKind values, one bit per kind (bit = the kind's value).
    using ParamKindSet = std::uint32_t;

    /**
     * @brief The declaration of one contribution family: one row of the table.
     *
     * Every text was checked against the code it names (card NOM N2a).
     */
    struct ContributionSpec {
        ContributionKind    kind;            ///< The family.
        const char*         name;            ///< Its name in the table and the JSON record ("data_code").
        ContributionChannel channel;         ///< Where it enters the normal equations.
        ContributionStage   stage;           ///< When, relative to the rank gate.
        ContributionSubject subject;         ///< What one instance is about.
        ParamKindSet        params;          ///< The parameter kinds its rows or folds touch.
        const char*         weight;          ///< The weight source: the knob or the named constant.
        const char*         builder;         ///< The function (and file) that builds it.
        bool                is_observation;  ///< True for rows of this epoch's observations.
    };

    /**
     * @brief The declaration table: one entry per kind, in enum order.
     * @return The table (static storage).
     */
    const std::array<ContributionSpec, kContributionKinds>& contributionSpecs();

    /**
     * @brief The declaration of a kind.
     * @param k The kind.
     * @return Its table entry (static storage).
     */
    const ContributionSpec& contributionSpec(ContributionKind k);

    /**
     * @brief The name of a kind.
     * @param k The kind.
     * @return @c contributionSpec(k).name, e.g. "data_code".
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
     * @brief Does a family touch a parameter kind?
     * @param spec The family's declaration.
     * @param k    The parameter kind.
     * @return True when @p k is in @c spec.params.
     */
    bool contributionTouches(const ContributionSpec& spec, NetworkParamKind k);

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
     * input row on the diagonal of L⁻¹ (its primary row).
     */
    struct RowTag {
        ContributionKind kind{ContributionKind::DataCode};  ///< The family.
        SatId            sat{};                             ///< The satellite, where the subject has one.
        SignalId         signal{};                          ///< The signal, where the subject has one.
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
     * @brief The tag of a row or prior on a receiver phase bias, from its key
     *        ("GPS_RPB_L2W", @c StateKeys::makeReceiverPhaseBiasKey).
     * @param kind The family.
     * @param key  The receiver phase-bias key.
     * @return The tag with the key's signal; no signal for a key that does not parse.
     */
    RowTag rxPhaseBiasTag(ContributionKind kind, std::string_view key);

    /**
     * @brief The tag of a row or prior on a receiver code bias, from its key: the signal of
     *        an inter-signal state ("GPS_ISC_C1C"); none for a DCB or an IFB key, whose
     *        subject is a band pair or a band, not a signal.
     * @param kind The family.
     * @param key  The receiver code-bias key.
     * @return The tag.
     */
    RowTag rxCodeBiasTag(ContributionKind kind, std::string_view key);

    /**
     * @brief The signals of a station's receiver inter-signal states (card SCC): the code
     *        rows of these signals route through such a state rather than the per-band DCB.
     *
     * The test the second-code weight pass of @c networkSplitStation_ applies, read from the
     * state set (the states @c StateInitializer::resolveReceiverBias allocated), so a tag
     * and a weight cannot disagree about which rows are companions.
     *
     * @param sm The station's state manager.
     * @return The code signals ("C1C") with a "<system>_ISC_<signal>" state.
     */
    std::set<std::string> receiverIscSignals(const StateManager& sm);

    /**
     * @brief Tag the data rows of a station block: one tag per row of the measurement update.
     *
     * The kind follows the weight passes of @c networkSplitStation_: a phase row is
     * @c DataPhaseSecond when its (satellite, signal) is in the second-phase cell set and
     * @c DataPhase otherwise; any other row is @c DataCodeCompanion when the second code
     * channel is active and its signal routes through a receiver inter-signal state, and
     * @c DataCode otherwise.  A row past the end of the identity vectors (an asserted
     * invariant says there is none) is tagged @c DataCode with no identity, so the tag
     * vector stays aligned.
     *
     * @param tags      The station's tag vector; cleared, then filled with @p m tags.
     * @param m         Rows of the measurement update (@c em.H.rows()).
     * @param sats      The rows' satellites (@c em.satellite_ids).
     * @param sigs      The rows' observation codes (@c em.signal_types).
     * @param isc_sigs  @ref receiverIscSignals of the station, or null when the second code
     *                  channel is not active.
     * @param spc_cells The station's second-phase cells (@c StateKeys::makeSatPhaseBiasKey).
     */
    void tagDataRows(std::vector<RowTag>& tags, Eigen::Index m,
                     const std::vector<std::string>& sats,
                     const std::vector<std::string>& sigs,
                     const std::set<std::string>* isc_sigs,
                     const std::set<std::string>& spc_cells);

    /**
     * @brief The tags of a station block after a within-epoch refloat re-whitens its
     *        jointly whitened coupling rows (card 1b): the rows before the coupling block,
     *        then the kept coupling rows' tags.
     *
     * The keep rule is @c whitenCouplingRows's: a row survives when its local column is not
     * in @p skip.  On a failed re-whiten the caller appends no coupling row, and @p nk is 0.
     *
     * @param tags The station's tags (aligned with its primary block).
     * @param nd   Rows before the coupling block (@c cpl_row_start).
     * @param raw  The coupling rows' pre-whitening ingredients (@c cpl_raw), with their tags.
     * @param skip The refloated local columns.
     * @param nk   Coupling rows the re-whiten produced.
     * @return The tags of the rebuilt block (size @p nd + @p nk when @p nk matches the kept
     *         rows; the caller compares the size with its rows).
     */
    std::vector<RowTag> rowTagsAfterJointRewhiten(const std::vector<RowTag>& tags,
                                                  Eigen::Index nd,
                                                  const std::vector<CplRowRaw>& raw,
                                                  const std::set<Eigen::Index>& skip,
                                                  Eigen::Index nk);

    /**
     * @brief Rows of a station block without a tag: the gap between the block's rows and
     *        its row tags, and between each diagonal prior channel and its tags.
     * @param R The station's assembly.
     * @return |rows(A_r) − row tags| + |carry entries − carry tags| + |prior entries − prior
     *         tags|; 0 when every row and entry is tagged.
     */
    long long untaggedRows(const StationAssembly& R);

    // =======================================================================
    // The shared folds of one epoch (written by the estimator)
    // =======================================================================

    /**
     * @brief One shared fold of the primary build: its kind (when the site named one) and
     *        its dimension (the priors, keys, rows, walks, gauges or blocks handed in).
     */
    struct SharedFoldRecord {
        ContributionKind kind{ContributionKind::DataCode};  ///< The kind the site named.
        bool             has_kind{false};                   ///< False: the site named none (the integrity line counts it).
        long long        dim{0};                            ///< The fold's dimension.
    };

    /**
     * @brief The shared folds of one epoch, kept by the estimator and cleared by its
     *        @c reset().
     */
    struct SharedFoldLog {
        std::vector<SharedFoldRecord> folds;  ///< The primary build's folds, in call order.
        long long replay_builds{0};           ///< Within-epoch replay builds (fresh arrowheads).
        long long replay_folds{0};            ///< Shared re-folds made by those builds.
        long long replay_dim{0};              ///< Their summed dimension.
        long long pins{0};                    ///< Clock pivot pins of the last primary solve.
        bool      solved{false};              ///< A primary solve ran this epoch.

        /// Empty the log (a new epoch).
        void clear() { *this = SharedFoldLog{}; }
    };

    /**
     * @brief The outcome of the GIM ionosphere row builder on one station-epoch (card NOM
     *        section 16, suspect 4: the builder ran silently inert on the golden).
     */
    enum class GimRowsOutcome : std::uint8_t {
        NotCalled = 0,  ///< The builder did not run for the station.
        NoGim,          ///< No GIM is loaded (the W6-none setup never builds one).
        WeightingOff,   ///< estimation.ionosphere_weighting is off.
        NoRows,         ///< Ran, and no line of sight qualified.
        Appended,       ///< Appended rows.
    };

    // =======================================================================
    // Tallies, the table and the seams
    // =======================================================================

    /**
     * @brief One kind's count: rows (station rows, diagonal prior entries, or the summed
     *        dimension of shared folds) and folds (station blocks carrying the kind, or
     *        shared fold calls).
     */
    struct ContributionCount {
        long long rows{0};   ///< Rows, entries or folded dimension.
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
        long long untagged_rows{0};           ///< @ref untaggedRows summed over them.
        long long coalesced_entries{0};       ///< Diagonal prior entries merged into another entry of the same column.
        long long folds_without_kind{0};      ///< Shared folds whose site named no kind.
        long long replay_builds{0};           ///< Within-epoch replay builds.
        long long replay_folds{0};            ///< Their shared re-folds (not in @ref kinds).
        long long replay_dim{0};              ///< Their summed dimension.
        long long replay_realigned{0};        ///< Station blocks whose tags a replay re-aligned (joint re-whiten).
        long long replay_misaligned{0};       ///< Of the replay's station blocks, those whose tags did not match the rows.
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
        long long untagged_rows_all{0};   ///< @ref untaggedRows over every assembled epoch, tallied or not.
        long long checked_all{0};         ///< Station blocks checked over every assembled epoch.
        long long replay_realigned{0};    ///< This epoch's replay re-alignments (reset at the station drain).
        long long replay_misaligned{0};   ///< This epoch's replay misalignments (reset at the station drain).
        bool      first_table_written{false};  ///< The first-emitted-epoch table is out.
    };

    /**
     * @brief The station side of an epoch, at the end of PASS B (serial): the rows and
     *        prior entries of every station the estimator took, per kind, and the tag
     *        check of every assembled station.
     *
     * Rows count for a station with an estimator index; its pre-PD prior entries count
     * with it (they fold in @c addStation), its post-PD entries when the consume folded
     * them (@c StationAssembly::carry_prior_folded).  Writes @c led.pending and the
     * run-wide integrity counters.
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

    /**
     * @brief The epoch tail: the shared folds of the epoch's primary build, merged with the
     *        station side into the epoch's tally and the run's; the table and its manifest
     *        record once, after the first emitted epoch.
     *
     * @param ctx     Run context (@c ctx.contributions).
     * @param est     The estimator (its @ref SharedFoldLog of this epoch).
     * @param log     The run logger.
     * @param traces  Trace streams (the run manifest).
     * @param nc      Network configuration (the manifest switch).
     * @param emitted True when this epoch reached the wire.
     */
    void networkContributionsEpochTail(NetworkRunContext& ctx, const NetworkEstimator& est,
                                       const std::shared_ptr<spdlog::logger>& log,
                                       NetworkRunTraces& traces, const NetworkConfig& nc,
                                       bool emitted);

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
     * @brief The "contributions as armed" table: a header line, one line per kind (every
     *        declared kind, a zero included), the GIM note, the integrity lines.
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

} // namespace GnssProcessor
