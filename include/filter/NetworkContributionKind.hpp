#pragma once
/**
 * @file NetworkContributionKind.hpp
 * @brief The kind of a contribution to the network producer's normal equations, and the
 *        estimator's log of one epoch's shared folds (card NOM step N2a,
 *        docs/refound/NOM_network_object_model_2026-10-08.md sections 3.4 and 17;
 *        docs/refound/NOM2_network_object_model_rows_2026-10-09.md).
 *
 * The small half of the contribution vocabulary: @c NetworkEstimator records the kind of
 * every shared fold and needs nothing else, so the estimator includes this header only.
 * The declaration of each kind (channel, stage, subject, unit, parameter kinds, weight
 * source, builder) and the row tags are in filter/NetworkContributions.hpp.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief One value per family of contribution to the normal equations, in the order of
     *        the declaration table (@c contributionSpecs in filter/NetworkContributions.hpp).
     *
     * The families are the inventory of card NOM sections 3.4 and 17, split wherever a
     * reader has to tell two cases apart (for example the whitened local block, one kind
     * per family of its primary column).  The run-start table "contributions as armed"
     * prints every family with its declaration, so the table is the inventory.
     *
     * Names (@c contributionKindName) read from the general to the particular: the group's
     * form where it has one (data, prior, block, coupling, cb, border_carry, q3, clone),
     * then the parameter or signal, then the role that tells two kinds of one parameter
     * apart (carry, datum, pin, anchor, ...).  Every station prior and every block row
     * names its role, so the four receiver code-bias kinds read apart:
     * @c prior_rx_cb_carry, @c prior_rx_cb_datum, @c prior_rx_cb_datum_pre_pd and
     * @c block_rx_cb_carry.
     *
     * @c Unset closes the enum.  It is not a family: it is the default of every tag and
     * fold record, so a row or fold that no site named is counted on its own integrity
     * line instead of passing as an observation row.
     */
    enum class ContributionKind : std::uint8_t {
        // --- Station data rows: the split's rows [0, em.H.rows()) ---------------------
        DataCode = 0,             ///< Code rows of the latched code signal.
        DataCodeCompanion,        ///< Code rows routed through a receiver inter-signal state (card SCC): the second code channel's companion rows and its fallback rows (an allow-listed band-1 code that arrived without the canonical code beside it).
        DataPhase,                ///< Phase rows of the latched phase signal.
        DataPhaseSecond,          ///< Phase rows of a second phase channel (card SID phase 6a).
        // --- Station constraint rows -------------------------------------------------
        AmbiguityPin,             ///< Phase-datum pins on the station's anchored ambiguities (legacy pivot, prioritized, eq34 tree edges).
        Eq34RpbPin,               ///< The eq34 datum receiver's receiver phase-bias pin.
        Eq34RpbRankFloor,         ///< The eq34 zero-mean receiver phase-bias rank floor.
        Eq34RpbTimeLink,          ///< The eq34 receiver phase-bias time link toward the carried value.
        GloIfbAnchor,             ///< Zero-mean anchor rows on the GLONASS per-slot receiver IFB columns.
        GloIonoAnchor,            ///< Zero-mean anchor rows on the GLONASS slant-ionosphere columns.
        SimZeroIono,              ///< Test-only rows tying every slant ionosphere to absolute zero (network.sim_zero_iono_sigma_m; card ARL, a fixture simulated without ionosphere).
        ShIonoTie,                ///< Slant-ionosphere to SH-VTEC tie rows (whitened per constellation).
        GimIono,                  ///< External GIM slant-ionosphere rows (whitened per constellation).
        SdEwl,                    ///< Between-satellite single-differenced EWL rows (whitened per constellation).
        // --- Station coupling and block rows: the coupling region (appendCouplingRows) ---
        CouplingScalar,           ///< A carried local column's conditional coupling row, +1 on the column (card 1).
        BlockAmbiguityCarry,      ///< Whitened local carry block row whose primary column is an ambiguity (F3).
        BlockZwdCarry,            ///< Whitened local carry block row whose primary column is the wet delay (B9-in-block).
        BlockIonoCarry,           ///< Whitened local carry block row whose primary column is a slant ionosphere (I-in-block).
        BlockRxClockCarry,        ///< Whitened local carry block row whose primary column is the receiver clock or an ISB (RX-CLK-in-block).
        BlockRxCbCarry,           ///< Whitened local carry block row whose primary column is a receiver code bias (RXCB-IN-BLOCK).
        BlockPhaseColourCarry,    ///< Whitened local carry block row whose primary column is a phase colour (card LGM).
        // --- Station phase-colour prior rows (card LGM) --------------------------------
        PhaseColourBirth,         ///< The stationary prior row of a phase-colour column with no live carry.
        PhaseColourBackstop,      ///< The stationary prior row of a carried phase-colour column whose carry did not fold.
        // --- Station priors (the diagonal channels) ------------------------------------
        PriorAmbiguityCarry,      ///< Diagonal carried prior on an ambiguity (leveling carry, diagonal route).
        PriorZwdCarry,            ///< Diagonal carried prior on the wet delay (B9).
        PriorRxCbCarry,           ///< Diagonal carried prior on a receiver code bias (rx-CB carry).
        PriorAmbiguityOraclePin,  ///< Simulation-only oracle ambiguity pin (diagonal).
        PriorRxCbDatum,           ///< Zero-mean receiver DCB datum on the post-PD channel (RXCB-DATUM, pre-PD routing off).
        PriorRxCbDatumPrePd,      ///< Zero-mean receiver DCB datum on the pre-PD channel (RXCB-DATUM, network.station_prior_before_pd).
        // --- Shared rows ---------------------------------------------------------------
        CbIfPin,                  ///< W6 code-bias datum: the per-satellite ionosphere-free pin row.
        CbSatGfPair,              ///< W6 code-bias datum: the per-satellite geometry-free pair row (sat_gf_datum).
        CbExtraBandHeld,          ///< W6 code-bias datum: an extra-band column held at its own level (B5D).
        CbExtraGf,                ///< W6 code-bias datum: an extra signal's geometry-free row (ISC asserted zero, RF-0d).
        CbSumToZero,              ///< W6 code-bias datum: the (constellation, signal) sum-to-zero row.
        CbFamilyBirth,            ///< Soft first-sighting family prior on a satellite's inter-family code-bias difference (card GFC step 2): one row per (satellite, birth), at 1/sigma^2, apart from the gauge rows.
        ClockConstraintRows,      ///< Constraint-realized clock datum rows (sum-to-zero or broadcast-anchored).
        SbasisCompletion,         ///< S-basis completion rows on the directions the declared datum misses (SBC).
        // --- Shared priors -------------------------------------------------------------
        CbKaula,                  ///< Loose physical-bound prior on the satellite code biases (card KP).
        PbHeal,                   ///< Drop-and-heal prior on a near-null phase-bias direction (the rank gate's response).
        ShPrior,                  ///< Carried SH-VTEC prior.
        PinBridge,                ///< eq34 pin bridge on an unpinned key (reattach hygiene).
        BorderCarryDiagonal,      ///< The border carry, diagonal form (D2, and the dense fold's leftovers).
        BorderCarryGaugeFree,     ///< The border carry, diagonal with the clock common mode downdated (card Q3 gauge-free fold).
        BorderCarryDense,         ///< The border carry, dense information block (C1; the clone path's stand-in block included).
        Q3Withdraw,               ///< The clock-anchor guard's withdrawal before its re-solve (card Q3): each call removes the tripped clocks' share of a carried prior (a refold re-folds the kept part in the same call).
        Q3Restore,                ///< The clock-anchor guard's restore after a re-solve that threw (card Q3): each call folds the withdrawn share back.
        ClonePrior,               ///< Isolated priors on idle clones (card SCL / BCL).
        CloneWalk,                ///< Clone walk factors (card SCL / PFQ).
        SharedHistory,            ///< N(0, 1) priors on the shared-history columns (card SCL2 X2).
        ClockOscillator,          ///< Two-state clock-model prior on the satellite clocks.
        ColdClockAnchor,          ///< A-priori satellite-clock anchor (cold-start conditioning).
        // --- Pins, gauges, DD blocks ---------------------------------------------------
        ClockPivotPin,            ///< Per-constellation clock pivot pins (Dirichlet, in the solve).
        DdPriorBlock,             ///< Card-4 DD prior blocks (carry_invariant).
        // --- Not a family ----------------------------------------------------------------
        Unset,                    ///< No site named a kind: the default of every tag and fold record.
    };

    /// Number of families: the @ref ContributionKind values before @c Unset (the size of the
    /// declaration table).
    inline constexpr std::size_t kContributionKinds =
        static_cast<std::size_t>(ContributionKind::Unset);

    /**
     * @brief One shared fold of the primary build: the kind its site named and its
     *        dimension (the count in the kind's unit: keys, rows, walks, gauges or blocks
     *        handed to the fold).
     */
    struct SharedFoldRecord {
        ContributionKind kind{ContributionKind::Unset};  ///< The kind the site named; @c Unset when it named none.
        long long        dim{0};                         ///< The fold's dimension.
    };

    /**
     * @brief The shared folds of one epoch, kept by @c NetworkEstimator and cleared by its
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

} // namespace GnssProcessor
