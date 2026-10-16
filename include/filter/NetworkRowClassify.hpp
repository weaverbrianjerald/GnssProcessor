#pragma once
/**
 * @file NetworkRowClassify.hpp
 * @brief The shared row classifier for every assembled network row, read from the row's
 *        declared contribution kind, and the shared-column family pass
 *        (INC-0 of docs/SPEC_prefit_census_observability.md; card NOM step N2b,
 *        docs/refound/NOM2_network_object_model_rows_2026-10-09.md).
 *
 * `classifyRow` and the shared-column family/label pass live here so that the
 * attribution (`src/core/filter/NetworkRhsAttrib.cpp`), the prefit census
 * (INC-1), the structural fuse (INC-2), the seed-birth tripwire (INC-3), the
 * topology ledger (INC-4), the redundancy census and its riders all use one verdict
 * about what a row is.
 *
 * ## Where a row's class comes from (card NOM step N2b)
 *
 * Every row of a station block carries a @ref RowTag, appended by the site that built
 * the row (card NOM step N2a, filter/NetworkContributions.hpp).  The class is a function
 * of the tag's kind (@ref rowClassOfKind), read in one place.  Before this step the
 * class was inferred: phase from a satellite phase-bias partial or a touch above 0.5 on
 * an ambiguity column listed in @c R.pl_amb, code from a satellite code-bias partial,
 * coupling from the row's position after @c R.cpl_row_start.  That inference filed the
 * phase rows of a station-unique (satellite, signal) as @c obs_other until the carry was
 * warm (no phase-bias column, an empty @c pl_amb) and the code rows of a station-unique
 * code signal as @c obs_other (no code-bias column); card NOM section 16, suspect 3.
 *
 * ## What a class means
 *
 *  - @c obs_code / @c obs_phase: observation rows, by their declared observation type
 *    (the code and phase kinds of the split, the companion code and second phase
 *    channels included).
 *  - @c obs_other: UNCLASSIFIED.  A row with no tag, an unset kind, or a kind that is
 *    not a station row (a diagonal prior or a shared kind tagged onto a row).  The
 *    contributions table counts these rows on its own integrity line (expected 0); a
 *    reader keeps them out of the code and phase pools.  The class keeps its index and
 *    its name because both are trace contracts (the census columns, the event text).
 *  - @c iono_sh_row: the SH-VTEC to slant-ionosphere tie rows.  A data class, but it
 *    carries no receiver clock, so the census does not common-mode-centre it.
 *  - @c pivot_pin / @c local_pin_row: the station constraint rows (ambiguity and eq34
 *    pins, GLONASS anchors, GIM ionosphere rows, SD-EWL rows) and the phase-colour prior
 *    rows: a hard gauge pin when the row's weight is at least
 *    @ref kNetPivotPinWeight, a soft local prior or anchor row otherwise (the weight
 *    rule the inference used for a row with no shared partial).
 *  - @c cpl_row: the coupling rows and the whitened carried-prior block rows
 *    (@ref contributionInCouplingRegion); they are statements about the carry, not data.
 *
 * Only the four data classes feed the census and the fuse (@ref isDataRowClass).  A
 * healthy pivot pin has a standardized deviation of about 1e7 by design and would
 * dominate any statistic it entered.
 *
 * ## The RAIM pool's admission rule
 *
 * The carried-phase RAIM pools (the post-fit pass, NetworkRaim.cpp, and the within-epoch
 * assess, NetworkSolveGlue.cpp) admit a row to the pool of an ambiguity column when its
 * coefficient on the column exceeds 0.5 and its kind is one @ref raimPoolAdmitsKind
 * declares: the observation phase rows, the coupling and block carried-prior rows, and the
 * SD-EWL constraint rows.
 * That is today's population stated as a rule (card NOM section 16, suspect 1): the
 * prior rows are in it on purpose until the science card RPA decides otherwise.  Every
 * admission is counted per kind (@c RaimPoolAdmissionTally, filter/NetworkRaim.hpp), and a
 * row of any other kind that clears 0.5 is counted as refused (expected 0).
 *
 * Card RPA's knob (@c network.raim_pool_data_rows_only, default off) replaces the rule in
 * both pools by @ref raimPoolAdmitsRow's second setting: the observation phase rows only,
 * each to the pool of the column its tag names.  The refused counts then hold the
 * carried-prior and SD-EWL rows that cleared 0.5.
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <Eigen/Dense>

#include "filter/NetworkContributions.hpp"   // RowTag, ContributionKind

namespace GnssProcessor {

class NetworkEstimator;
struct StationAssembly;

/// Family tag of a shared column, read once per epoch from the estimator's column
/// identity (@ref sharedColumnFamily).  The families match the label prefixes of
/// NetworkEstimator::sharedColumnLabel ("SH" / "PB:" / "CB:" / "CLK:"); a clone of any
/// kind (a clock clone "CLKC:", card SCL; a bias clone "PBC:" or "CBC:", card BCL) and a
/// shared-history column ("HIST:") are @c Other.
enum class NetColFam : char { Sh, Pb, Cb, Clk, Other };

/**
 * @brief The family of one shared column, and its satellite, from the estimator's
 *        column identity (@c NetworkEstimator::sharedColumnIdentity).
 *
 * A satellite column (clock, code bias, phase bias) gives @c Clk, @c Cb or @c Pb and
 * its satellite.  A column with no satellite identity is an SH-VTEC coefficient
 * (@c Sh, either shell) unless it is a clone (a clock clone, card SCL, or a phase- or
 * code-bias clone, card BCL) or a shared-history column (card SCL2 X2), which are
 * @c Other, as is a column out of range.  A bias clone is not a bias of its family: its
 * label ("PBC:", "CBC:") never matched the "PB:" and "CB:" prefixes this function
 * replaced, and its column identity is withheld (@c sharedColumnIdentity is false).
 *
 * @param est The estimator whose current epoch layout is being classified.
 * @param col The shared column.
 * @param sat Optional out: the column's satellite, written for @c Clk, @c Cb and
 *            @c Pb only.  Pass nullptr to skip.
 * @return The family.
 */
NetColFam sharedColumnFamily(const NetworkEstimator& est, Eigen::Index col,
                             std::string* sat = nullptr);

/// Row class.  The enumerator order of the four data classes is the census's
/// column order (@ref dataClassIndex); reordering them changes the trace header.
enum class NetRowClass : unsigned char {
    ObsCode = 0,     ///< @c obs_code
    ObsPhase = 1,    ///< @c obs_phase
    ObsOther = 2,    ///< @c obs_other: unclassified (no tag, an unset kind, or a kind that is not a station row)
    IonoShRow = 3,   ///< @c iono_sh_row
    PivotPin = 4,    ///< @c pivot_pin
    LocalPinRow = 5, ///< @c local_pin_row
    CplRow = 6       ///< @c cpl_row
};

/// Number of data classes (the census/fuse domain).
inline constexpr int kNetDataClasses = 4;

/// The weight at and above which a station constraint row is a hard gauge pin
/// (@c pivot_pin) rather than a soft local prior row (@c local_pin_row).  The ambiguity
/// and eq34 pins fold at 1e10.
inline constexpr double kNetPivotPinWeight = 1e9;

/// @return Whether @p c is a data class: an observation equation or an
///         SH↔slant tie row.  Pins and coupling rows are excluded.
inline constexpr bool isDataRowClass(NetRowClass c) {
    return static_cast<unsigned char>(c) < static_cast<unsigned char>(kNetDataClasses);
}

/// @return Census column index of a data class (0..3), or −1 for pins/coupling.
inline constexpr int dataClassIndex(NetRowClass c) {
    return isDataRowClass(c) ? static_cast<int>(c) : -1;
}

/// @return The class's stable wire name (the string the attribution dumps and
///         the census events have always used).  Never null.
const char* rowClassName(NetRowClass c);

/**
 * @brief Is a kind an observation code row: @c DataCode (the latched code signal) or
 *        @c DataCodeCompanion (the second code channel's companion and fallback rows)?
 * @param k The kind.
 * @return True for those two kinds.
 */
bool isCodeObservationKind(ContributionKind k);

/**
 * @brief Is a kind an observation phase row: @c DataPhase (the latched phase signal) or
 *        @c DataPhaseSecond (a second phase channel)?
 * @param k The kind.
 * @return True for those two kinds.
 */
bool isPhaseObservationKind(ContributionKind k);

/**
 * @brief The class every reader files a row of a declared kind under: the one map from
 *        contribution kind to row class.
 *
 * | kinds | class |
 * | --- | --- |
 * | @c DataCode, @c DataCodeCompanion | @c obs_code |
 * | @c DataPhase, @c DataPhaseSecond | @c obs_phase |
 * | @c ShIonoTie | @c iono_sh_row |
 * | @c AmbiguityPin, the three eq34 rows, both GLONASS anchors, @c GimIono, @c SdEwl, @c PhaseColourBirth, @c PhaseColourBackstop | @c pivot_pin when @p w >= @ref kNetPivotPinWeight, else @c local_pin_row |
 * | @c CouplingScalar, the six @c Block*Carry kinds | @c cpl_row |
 * | every other kind (a diagonal prior or a shared kind, which no row should carry), and @c Unset | @c obs_other (unclassified) |
 *
 * @param k The row's kind.
 * @param w The row's information weight (read for the constraint rows only).
 * @return The class.
 */
NetRowClass rowClassOfKind(ContributionKind k, double w);

/**
 * @brief The tag of one row of a station block.
 * @param R   The station's assembly.
 * @param row Row index into @c R.A_s / @c R.A_r.
 * @return @c R.row_tags[row], or a default tag (@c Unset, no satellite, no signal) when
 *         the block has no tag for the row (counted by the contributions table's
 *         "station rows without a tag" line).
 */
const RowTag& rowTagAt(const StationAssembly& R, Eigen::Index row);

/**
 * @brief The observation code of an observation row, from its tag ("C1W", "L1C").
 * @param t The row's tag.
 * @return The code of the tag's signal under the kind's observation type; empty for a
 *         kind that is not an observation or a tag without a signal.
 */
std::string rowObservationCode(const RowTag& t);

/**
 * @brief Classify one assembled row of one station from its tag.
 *
 * @param R       The station's assembled block.
 * @param row     Row index into @c R.A_s / @c R.A_r / @c R.w / @c R.y.  A row
 *                beyond @c R.w is treated as weight 1.
 * @param row_sat Optional out: the satellite the row's tag names (its line of sight;
 *                for a whitened row, its primary row's), written only while still empty
 *                and only when the tag names one, so a caller may pre-seed it.  Pass
 *                nullptr to skip.
 * @return @ref rowClassOfKind of the row's tag (@ref rowTagAt) and weight.
 */
NetRowClass classifyRow(const StationAssembly& R, Eigen::Index row, std::string* row_sat);

/**
 * @brief The carried-phase RAIM pools' declared admission rule: may a row of this kind
 *        enter the pool of an ambiguity column whose coefficient on it exceeds 0.5?
 *
 * Admitted: the observation phase rows (@c DataPhase, @c DataPhaseSecond, which carry
 * exactly +1 on their ambiguity column), the carried-prior rows of the coupling region
 * (@c CouplingScalar and the six @c Block*Carry kinds: a whitened block
 * row is one row of L⁻¹ and touches the ambiguity columns pushed before its own), and the
 * SD-EWL constraint rows (@c SdEwl: whitened between-satellite rows on the ambiguity
 * columns, which the SD-EWL smoke of card NOM step N2b measured clearing 0.5).  The rule
 * reproduces the population the coefficient test alone admitted before card NOM step
 * N2b; the prior and constraint rows are kept on purpose (card NOM section 16, suspect
 * 1, and the science card RPA, which decides whether they stay).
 *
 * @param k The row's kind.
 * @return True for the eleven admitted kinds.
 */
bool raimPoolAdmitsKind(ContributionKind k);

/**
 * @brief The subject of a carried ambiguity column of a RAIM pool: its satellite and
 *        signal as a tag, for @ref raimPoolAdmitsRow (card RPA).
 *
 * @param sat    The pool entry's RINEX satellite id (@c PlAmb::sat, "G05").
 * @param signal The pool entry's signal (@c PlAmb::signal, the ambiguity key's signal,
 *               "L1W").
 * @return A tag of kind @c DataPhase naming that satellite and signal; a satellite or a
 *         signal that does not parse leaves that field default, and then no row's tag
 *         names the column.
 */
RowTag raimPoolColumnSubject(std::string_view sat, std::string_view signal);

/**
 * @brief Does a row's tag name a column's satellite and signal?
 *
 * @param row    The row's tag.
 * @param column The column's subject (@ref raimPoolColumnSubject).
 * @return True when both tags name the same valid satellite and the same signal.  A
 *         default (unparsed) satellite or signal names nothing.
 */
bool rowTagNamesColumn(const RowTag& row, const RowTag& column);

/**
 * @brief The carried-phase RAIM pools' admission of one row to the pool of one ambiguity
 *        column, under both settings of @c network.raim_pool_data_rows_only (card RPA,
 *        docs/refound/RPA_raim_pool_data_rows_2026-10-08.md).
 *
 *  - **Off** (today's declared rule): the row's coefficient on the column exceeds 0.5 and
 *    @ref raimPoolAdmitsKind admits its kind, so a whitened carried-prior row enters the
 *    pool of every column it clears.
 *  - **On**: the row is an observation phase row (@ref isPhaseObservationKind) whose tag
 *    names the column's satellite and signal (@ref rowTagNamesColumn).  The coefficient
 *    is not read: an observation phase row carries +1 on its own ambiguity column and
 *    nothing on any other, and the pools count, per phase row, every column on which the
 *    coefficient test and the tag disagree (@c RaimPoolAdmissionTally::noteOwnColumn,
 *    0 expected).  The carried-prior rows and the SD-EWL rows enter no pool.
 *
 * @param row            The row's tag.
 * @param clears         Whether the row's coefficient on the column exceeds 0.5 in
 *                       magnitude (read with the knob off only).
 * @param column         The column's subject (@ref raimPoolColumnSubject; read with the
 *                       knob on only).
 * @param data_rows_only The knob, as resolved for the run (@c NetworkKnobs).
 * @return Whether the row enters the column's pool.
 */
bool raimPoolAdmitsRow(const RowTag& row, bool clears, const RowTag& column,
                       bool data_rows_only);

/**
 * @brief One epoch's shared-column labels, families, per-column satellite, and
 *        the two SH mean columns, built once per epoch for the readers that need a
 *        column's identity (the attribution's per-family norms and its target columns,
 *        the topology ledger's column events).
 *
 * The shared layout is rebuilt every epoch, so this map is not valid across
 * epochs.  A label lookup that throws falls back to @c "col<index>" (family
 * @c Other) instead of aborting the walk, so a diagnostic degrades instead of
 * stopping the run.  A row's class does not read it (@ref classifyRow reads the tag).
 */
struct SharedColumnMap {
    Eigen::Index             n{0};        ///< number of shared columns.
    std::vector<std::string> label;       ///< per-column label (size @ref n).
    std::vector<NetColFam>   fam;         ///< per-column family (size @ref n).
    std::vector<std::string> col_sat;     ///< per-column satellite id (CLK / CB / PB columns), empty for SH / unparsed.
    Eigen::Index             c00_l1{-1};  ///< column of @c SH:C:0:0  (−1 = absent).
    Eigen::Index             c00_l2{-1};  ///< column of @c SH2:C:0:0 (−1 = absent).
};

/**
 * @brief Build the epoch's shared-column map from the estimator's labels (kept for the
 *        traces) and column identities (the families, the satellites and the two SH
 *        mean columns).
 * @param est The estimator whose current epoch layout is being classified.
 * @param n   Shared-column count (@c est.numShared()); a non-positive value
 *            yields an empty map.
 */
SharedColumnMap buildSharedColumnMap(const NetworkEstimator& est, Eigen::Index n);

} // namespace GnssProcessor
